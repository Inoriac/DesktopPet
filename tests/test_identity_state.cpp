#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QTest>
#include <QTemporaryDir>

#include <memory>

#include "ai/ai_brain.h"
#include "ai/event/event_outbox.h"
#include "ai/event/event_ledger.h"
#include "ai/event/event_schema_registry.h"
#include "ai/event/runtime_unit_of_work.h"
#include "ai/event/sqlite_event_repository.h"
#include "ai/identity/personality_service.h"
#include "ai/identity/persona_projector.h"
#include "ai/identity/relationship_service.h"
#include "ai/identity/self_model_service.h"
#include "ai/identity/sqlite_identity_repository.h"
#include "ai/integration/emotion_state_provider.h"
#include "ai/runtime/agent_bootstrap.h"
#include "ai/runtime/agent_runtime_services.h"
#include "ai/runtime/runtime_ui_bridge.h"
#include "configLoader/config_manager.h"

namespace {

const QString kProfileId = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QDateTime kWindowEnd = QDateTime::fromString(
    QStringLiteral("2026-08-25T08:00:00.000Z"), Qt::ISODateWithMs);

TraitEvidenceDraft traitEvidence(const QString& sourceId,
                                 const QString& contextKey,
                                 double direction = 1.0,
                                 const QDateTime& at = kWindowEnd.addDays(-15)) {
    TraitEvidenceDraft evidence;
    evidence.trait = QStringLiteral("sociability");
    evidence.direction = direction;
    evidence.weight = 1.0;
    evidence.confidence = 1.0;
    evidence.contextKey = contextKey;
    evidence.sourceEventId = sourceId;
    evidence.createdAt = at;
    return evidence;
}

PersonalityConsolidationRequest consolidationRequest(int expectedVersion = 0) {
    PersonalityConsolidationRequest request;
    request.profileId = kProfileId;
    request.windowStart = kWindowEnd.addDays(-21);
    request.windowEnd = kWindowEnd;
    request.expectedVersion = expectedVersion;
    return request;
}

class StaticEmotionProvider final : public EmotionStateProvider {
public:
    ProvidedEmotionSnapshot snapshot;

    ProvidedEmotionSnapshot currentSnapshot(
        const QString& profileId, const QDateTime& at) const override {
        ProvidedEmotionSnapshot value = snapshot;
        value.profileId = profileId;
        value.value.updatedAt = at;
        return value;
    }

    QList<ProvidedEmotionSnapshot> trajectory(
        const QString&, const QDateTime&, const QDateTime&) const override {
        return {};
    }
};

class ConflictingIdentityRepository final : public SqliteIdentityRepository {
public:
    int appendAttempts = 0;

    Result<void, DomainError> appendPersonalityState(
        RuntimeUnitOfWork&, const PersonalitySnapshot&,
        const QStringList&) override {
        ++appendAttempts;
        return Result<void, DomainError>::failure(
            domainError(QStringLiteral("STATE_VERSION_CONFLICT"),
                        QStringLiteral("injected conflict")));
    }
};

struct IdentityFixture {
    QTemporaryDir directory;
    QString databasePath;
    SqliteEventRepository schemaRepository;
    EventSchemaRegistry schemas;
    SqliteIdentityRepository repository;
    std::unique_ptr<SqliteRuntimeUnitOfWorkFactory> unitOfWorkFactory;
    std::unique_ptr<SqliteEventOutbox> outbox;
    std::unique_ptr<PersonalityService> personality;
    std::unique_ptr<RelationshipService> relationship;
    std::unique_ptr<SelfModelService> selfModel;

    explicit IdentityFixture(SqliteIdentityRepository* customRepository = nullptr)
        : databasePath(directory.filePath(QStringLiteral("agent_runtime.sqlite"))) {
        SqliteIdentityRepository* activeRepository = customRepository
            ? customRepository : &repository;
        if (!directory.isValid() || !schemaRepository.open(databasePath).isOk()
            || !activeRepository->open(databasePath).isOk()
            || !registerBuiltInEventSchemas(schemas).isOk()) {
            return;
        }
        schemas.freeze();
        unitOfWorkFactory = std::make_unique<SqliteRuntimeUnitOfWorkFactory>(databasePath);
        outbox = std::make_unique<SqliteEventOutbox>(databasePath, &schemas, kProfileId);
        PersonalityPolicy policy;
        personality = std::make_unique<PersonalityService>(
            kProfileId, IdentityBaseline::defaults(), policy, activeRepository,
            unitOfWorkFactory.get(), outbox.get());
        relationship = std::make_unique<RelationshipService>(kProfileId, activeRepository);
        selfModel = std::make_unique<SelfModelService>(kProfileId, activeRepository);
    }

    bool ready() const {
        return directory.isValid() && unitOfWorkFactory && outbox && personality;
    }
};

std::unique_ptr<CallbackRuntimeUiBridge> makeBridge() {
    RuntimeUiCallbacks callbacks;
    callbacks.showChatBubble = [](const QString&, int) {};
    callbacks.notifyUser = [](const QString&, const QString&, int) {};
    return std::make_unique<CallbackRuntimeUiBridge>(
        std::move(callbacks));
}

struct RuntimeFixture {
    QTemporaryDir directory;
    AIBrain brain;
    std::unique_ptr<CallbackRuntimeUiBridge> bridge = makeBridge();
    StaticEmotionProvider emotion;
    AgentRuntimeServices services;
    RuntimeStartRequest request;
    Result<RuntimeStartReport, DomainError> result;

    RuntimeFixture()
        : request()
        , result(Result<RuntimeStartReport, DomainError>::failure(
              domainError(QStringLiteral("NOT_STARTED"), QStringLiteral("not started")))) {
        emotion.snapshot.schemaVersion = 1;
        emotion.snapshot.value.active = EmotionType::Joy;
        emotion.snapshot.value.intensity = 0.8;
        emotion.snapshot.value.moodValence = 0.7;
        emotion.snapshot.value.moodArousal = 0.6;
        request.profile = {QStringLiteral("Milltina"), QStringLiteral("model.gltf"), kProfileId};
        request.profileMigration.profileId = kProfileId;
        request.profileMigration.registeredProfileIds = {kProfileId};
        request.profileMigration.appDataRoot = directory.path();
        request.profileMigration.legacyDatabasePath =
            directory.filePath(QStringLiteral("legacy-memory.db"));
        request.profileMigration.legacyJsonPath =
            directory.filePath(QStringLiteral("legacy-memory.json"));
        request.configHash = QString(64, QLatin1Char('a'));
        request.identityBaseline = IdentityBaseline::defaults();
        request.identityBaselineSchemaVersion = request.identityBaseline.schemaVersion;
        request.identityBaselineHash = QString(64, QLatin1Char('b'));
        request.personalityPolicy = PersonalityPolicy{};
        request.emotionStateProvider = &emotion;
        request.aiBrain = &brain;
        request.uiBridge = bridge.get();
        result = AgentBootstrap::start(services, request);
    }

    bool ready() const {
        return directory.isValid() && result.isOk()
            && result.value().capabilities.profileGrowth;
    }
};

void addConsolidationEvidence(PersonalityService& service,
                              const QString& prefix = QStringLiteral("source")) {
    QVERIFY(service.recordEvidence(traitEvidence(prefix + QStringLiteral("-1"),
                                                  QStringLiteral("conversation"))).isOk());
    QVERIFY(service.recordEvidence(traitEvidence(prefix + QStringLiteral("-2"),
                                                  QStringLiteral("planning"), 1.0, kWindowEnd.addDays(-8))).isOk());
    QVERIFY(service.recordEvidence(traitEvidence(prefix + QStringLiteral("-3"),
                                                  QStringLiteral("reflection"), 1.0, kWindowEnd.addDays(-1))).isOk());
}

} // namespace

class TestIdentityState : public QObject {
    Q_OBJECT

private slots:
    void recordEvidence_whenExplicitCorrectionArrives_shouldPersistTraceableWeightedEvidence();
    void recordEvidence_whenSameSourceIsRepeated_shouldDeduplicateEvidence();
    void consolidate_whenIndependentLongWindowEvidencePassesThreshold_shouldAppendLimitedStateVersion();
    void consolidate_whenOnlyNeutralEmotionExists_shouldNotMoveBaseline();
    void consolidate_whenVersionConflicts_shouldLeaveEvidencePending();
    void rollback_whenTargetVersionExists_shouldAppendRestoredVersionWithoutDeletingHistory();
    void applyEvidence_whenOwnerEvidenceArrives_shouldChangeOnlyOwnerRelationship();
    void evolve_whenProposalHasCommittedIndependentEvidence_shouldAppendNarrativeVersion();
    void evolve_whenNarrativeReferencesOnlyItself_shouldRejectCircularEvidence();
    void project_whenSessionSnapshotIsValid_shouldMergeBaselineRelationshipAndBoundedSlots();
    void project_whenNullEmotionProviderUsed_shouldOmitEmotionPromptAndPrivateInternals();
    void getPersonalityPolicy_whenConfigured_shouldReturnSanitizedLimits();
    void getPersonalityPolicy_whenMissing_shouldUseSafeDefaults();
    void start_whenIdentityDependenciesAreValid_shouldExposeServicesAndRegisterPersonalityEvent();
    void captureSnapshot_whenIdentityVersionsExist_shouldPinSubjectSpecificCommittedVersions();
    void baseline_hasSevenTraitsAndBoundedEffectiveValues();
    void evidence_derivativeSourcesCannotMultiplyOriginalExperience();
    void consolidate_requiresActualEvidenceSpanAndCommitCadence();
    void consolidate_enforcesEveryTraitCapAndPersistsCoherentSummaries();
    void consolidate_respectsFrozenEventCutoff();
    void commit_rejectsStalePreviewWithoutConsumingNewEvidence();
    void commit_outboxFailureRollsBackStateAndConsumption();
    void relationship_dailyAbsoluteBudgetSurvivesRestartAndReversal();
    void runtime_taskCommandsDoNotCreateFeedbackEvidenceOrDuplicateFamiliarity();
    void evidence_revisesUnconsumedMeaningWhilePreservingOriginalIdentity();
    void commit_rejectsEvidenceRevisionEvenWhenFinalTraitsMatch();
};

void TestIdentityState::evidence_revisesUnconsumedMeaningWhilePreservingOriginalIdentity() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    auto evidence = traitEvidence(QStringLiteral("original-event"), QStringLiteral("conversation"), 0.0);
    evidence.sourceSequence = 17;
    QVERIFY(fixture.personality->recordEvidence(evidence).isOk());
    const auto original = fixture.repository.evidenceBySource(kProfileId, evidence.sourceEventId).value().first();
    QCOMPARE(original.status, TraitEvidenceStatus::Rejected);
    evidence.originalSourceId = original.originalSourceId;
    evidence.originalOccurredAt = original.originalOccurredAt;
    evidence.sourceEventId = QStringLiteral("new-diary-interpretation");
    evidence.sourceCategory = QStringLiteral("reflection");
    evidence.sourceSequence = 0; // Missing metadata must not erase a known cutoff.
    evidence.createdAt = kWindowEnd;
    evidence.direction = -1.0;
    evidence.weight = 0.5;
    evidence.confidence = 0.8;
    evidence.contextKey = QStringLiteral("planning");
    evidence.category = TraitEvidenceCategory::Feeling;
    QVERIFY(fixture.personality->recordEvidence(evidence).isOk());
    auto revised = fixture.repository.evidenceBySource(kProfileId, evidence.sourceEventId).value().first();
    QCOMPARE(revised.evidenceId, original.evidenceId);
    QCOMPARE(revised.originalSourceId, original.originalSourceId);
    QCOMPARE(revised.originalOccurredAt, original.originalOccurredAt);
    QCOMPARE(revised.createdAt, original.createdAt);
    QCOMPARE(revised.sourceSequence, 17);
    QCOMPARE(revised.direction, -1.0);
    QCOMPARE(revised.weight, 0.5);
    QCOMPARE(revised.confidence, 0.8);
    QCOMPARE(revised.contextKey, QStringLiteral("planning"));
    QCOMPARE(revised.category, TraitEvidenceCategory::Feeling);
    QCOMPARE(revised.status, TraitEvidenceStatus::Pending);
    QCOMPARE(revised.revision, 2);
    QVERIFY(fixture.personality->recordEvidence(evidence).isOk());
    QCOMPARE(fixture.repository.evidenceBySource(kProfileId, evidence.sourceEventId).value().first().revision, 2);
    evidence.originalOccurredAt = original.originalOccurredAt.addDays(1);
    QVERIFY(!fixture.personality->recordEvidence(evidence).isOk());
    evidence.originalOccurredAt = original.originalOccurredAt;
    evidence.sourceSequence = 18;
    QVERIFY(!fixture.personality->recordEvidence(evidence).isOk());
    evidence.sourceSequence = 17;
    evidence.direction = 0.0;
    QVERIFY(fixture.personality->recordEvidence(evidence).isOk());
    revised = fixture.repository.evidenceBySource(kProfileId, evidence.sourceEventId).value().first();
    QCOMPARE(revised.status, TraitEvidenceStatus::Rejected);
    QCOMPARE(revised.revision, 3);
}

void TestIdentityState::commit_rejectsEvidenceRevisionEvenWhenFinalTraitsMatch() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    addConsolidationEvidence(*fixture.personality);
    const auto evaluated = fixture.personality->preview(consolidationRequest());
    QVERIFY(evaluated.isOk() && evaluated.value().hasChanges);
    auto revised = static_cast<const TraitEvidenceDraft&>(fixture.repository.evidenceBySource(
        kProfileId, QStringLiteral("source-1")).value().first());
    revised.category = TraitEvidenceCategory::Feeling;
    QVERIFY(fixture.personality->recordEvidence(revised).isOk());
    const auto fresh = fixture.personality->preview(consolidationRequest());
    QVERIFY(fresh.isOk());
    QCOMPARE(fresh.value().candidate.tendencies, evaluated.value().candidate.tendencies);
    const auto stale = fixture.personality->commit(evaluated.value(), QStringLiteral("旧摘要"), QStringLiteral("旧叙事"));
    QVERIFY(!stale.isOk());
    QCOMPARE(stale.error().code, QStringLiteral("STATE_VERSION_CONFLICT"));
    QVERIFY(fixture.personality->commit(fresh.value(), {}, {}).isOk());
    const auto consumed = fixture.repository.evidenceBySource(kProfileId, revised.sourceEventId).value().first();
    revised.direction = -1.0;
    QVERIFY(fixture.personality->recordEvidence(revised).isOk());
    const auto unchanged = fixture.repository.evidenceBySource(kProfileId, revised.sourceEventId).value().first();
    QCOMPARE(unchanged.status, TraitEvidenceStatus::Consumed);
    QCOMPARE(unchanged.direction, consumed.direction);
    QCOMPARE(unchanged.revision, consumed.revision);
    QCOMPARE(unchanged.consumedVersion, consumed.consumedVersion);
}

void TestIdentityState::runtime_taskCommandsDoNotCreateFeedbackEvidenceOrDuplicateFamiliarity() {
    RuntimeFixture fixture;
    QVERIFY(fixture.ready());
    EventDraft command;
    command.profileId = kProfileId;
    command.type = QStringLiteral("UserMessageReceived");
    command.source = QStringLiteral("test");
    command.occurredAt = kWindowEnd;
    command.payload = {{QStringLiteral("text"), QStringLiteral("谢谢你")},
        {QStringLiteral("triggerTag"), QStringLiteral("user_request")}, {QStringLiteral("task_related"), true}};
    const auto storedCommand = fixture.services.eventLedger()->append(command);
    QVERIFY(storedCommand.isOk());
    QVERIFY(fixture.services.processIdentityEvents(kWindowEnd.addSecs(1)).isOk());
    QVERIFY(!fixture.services.captureSnapshot(QStringLiteral("after-command")).relationshipVersion.has_value());
    SqliteIdentityRepository repository;
    QVERIFY(repository.open(fixture.services.chatPreparationRuntimeMetadata().runtimeDatabasePath).isOk());
    QVERIFY(repository.evidenceBySource(kProfileId, storedCommand.value().eventId).value().isEmpty());

    EventDraft experience;
    experience.profileId = kProfileId;
    experience.type = QStringLiteral("TaskExperienceRecorded");
    experience.source = QStringLiteral("test");
    experience.occurredAt = kWindowEnd;
    experience.payload = {{QStringLiteral("task_id"), QStringLiteral("task-source")},
        {QStringLiteral("goal"), QStringLiteral("整理旅行方案")}, {QStringLiteral("status"), QStringLiteral("Completed")},
        {QStringLiteral("feedback_status"), QStringLiteral("unknown")}, {QStringLiteral("subject_id"), QStringLiteral("owner")}};
    QVERIFY(fixture.services.eventLedger()->append(experience).isOk());
    QVERIFY(fixture.services.processIdentityEvents(kWindowEnd.addSecs(2)).isOk());
    const auto relationship = repository.currentRelationship(kProfileId, QStringLiteral("owner"));
    QVERIFY(relationship.isOk() && relationship.value().has_value());
    QCOMPARE(relationship.value()->version, 1);
    QCOMPARE(relationship.value()->tendencies.value(QStringLiteral("familiarity")), 0.105);
    QCOMPARE(relationship.value()->tendencies.value(QStringLiteral("affinity")), 0.50);
}

void TestIdentityState::
recordEvidence_whenExplicitCorrectionArrives_shouldPersistTraceableWeightedEvidence() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    TraitEvidenceDraft evidence = traitEvidence(
        QStringLiteral("correction-event"), QStringLiteral("owner-feedback"), -1.0);
    evidence.weight = 9.0;

    QVERIFY(fixture.personality->recordEvidence(evidence).isOk());
    const auto stored = fixture.repository.evidenceBySource(
        kProfileId, QStringLiteral("correction-event"));
    QVERIFY(stored.isOk());
    QCOMPARE(stored.value().size(), 1);
    QCOMPARE(stored.value().first().sourceEventId, QStringLiteral("correction-event"));
    QCOMPARE(stored.value().first().weight, 1.0);
    QCOMPARE(stored.value().first().status, TraitEvidenceStatus::Pending);
}

void TestIdentityState::
recordEvidence_whenSameSourceIsRepeated_shouldDeduplicateEvidence() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    const TraitEvidenceDraft evidence = traitEvidence(
        QStringLiteral("same-event"), QStringLiteral("conversation"));

    QVERIFY(fixture.personality->recordEvidence(evidence).isOk());
    QVERIFY(fixture.personality->recordEvidence(evidence).isOk());
    const auto stored = fixture.repository.evidenceBySource(kProfileId, evidence.sourceEventId);
    QVERIFY(stored.isOk());
    QCOMPARE(stored.value().size(), 1);
}

void TestIdentityState::
consolidate_whenIndependentLongWindowEvidencePassesThreshold_shouldAppendLimitedStateVersion() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    addConsolidationEvidence(*fixture.personality);

    const auto result = fixture.personality->consolidate(consolidationRequest());

    QVERIFY(result.isOk());
    QCOMPARE(result.value().version, 1);
    QVERIFY(result.value().tendencies.value(QStringLiteral("sociability")) > 0.0);
    QVERIFY(result.value().tendencies.value(QStringLiteral("sociability")) <= 0.05);
    const auto evidence = fixture.repository.pendingEvidence(
        kProfileId, kWindowEnd.addDays(-21), kWindowEnd);
    QVERIFY(evidence.isOk());
    QVERIFY(evidence.value().isEmpty());
}

void TestIdentityState::
consolidate_whenOnlyNeutralEmotionExists_shouldNotMoveBaseline() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());

    const auto result = fixture.personality->consolidate(consolidationRequest());

    QVERIFY(result.isOk());
    QCOMPARE(result.value().version, 0);
    QVERIFY(result.value().tendencies.isEmpty());
}

void TestIdentityState::
consolidate_whenVersionConflicts_shouldLeaveEvidencePending() {
    ConflictingIdentityRepository repository;
    IdentityFixture fixture(&repository);
    QVERIFY(fixture.ready());
    addConsolidationEvidence(*fixture.personality, QStringLiteral("conflict"));

    const auto result = fixture.personality->consolidate(consolidationRequest());

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("STATE_VERSION_CONFLICT"));
    QCOMPARE(repository.appendAttempts, 1);
    const auto pending = repository.pendingEvidence(
        kProfileId, kWindowEnd.addDays(-21), kWindowEnd);
    QVERIFY(pending.isOk());
    QCOMPARE(pending.value().size(), 3);
}

void TestIdentityState::
rollback_whenTargetVersionExists_shouldAppendRestoredVersionWithoutDeletingHistory() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    addConsolidationEvidence(*fixture.personality);
    const auto first = fixture.personality->consolidate(consolidationRequest());
    QVERIFY(first.isOk());

    const auto rolledBack = fixture.personality->rollback(
        first.value().stateId, first.value().version, QStringLiteral("operator recovery"));

    QVERIFY(rolledBack.isOk());
    QCOMPARE(rolledBack.value().version, 2);
    QCOMPARE(rolledBack.value().tendencies, first.value().tendencies);
    const auto history = fixture.repository.personalityHistory(kProfileId);
    QVERIFY(history.isOk());
    QCOMPARE(history.value().size(), 2);
}

void TestIdentityState::
applyEvidence_whenOwnerEvidenceArrives_shouldChangeOnlyOwnerRelationship() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    RelationshipEvidence evidence;
    evidence.tendency = QStringLiteral("affinity");
    evidence.direction = 1.0;
    evidence.weight = 0.8;
    evidence.confidence = 1.0;
    evidence.sourceEventId = QStringLiteral("owner-event");
    evidence.occurredAt = kWindowEnd;

    const auto owner = fixture.relationship->applyEvidence(QStringLiteral("owner"), evidence);

    QVERIFY(owner.isOk());
    QCOMPARE(owner.value().version, 1);
    QVERIFY(owner.value().tendencies.value(QStringLiteral("affinity")) > 0.50);
    const auto unknown = fixture.repository.currentRelationship(
        kProfileId, QStringLiteral("unknown"));
    QVERIFY(unknown.isOk());
    QVERIFY(!unknown.value().has_value());
}

void TestIdentityState::
evolve_whenProposalHasCommittedIndependentEvidence_shouldAppendNarrativeVersion() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    SelfModelProposal proposal;
    proposal.narrative = QStringLiteral("我和主人相处时更愿意表达。 ");
    proposal.proposedAt = kWindowEnd;
    EvidenceSet evidence;
    evidence.items = {
        {QStringLiteral("event-1"), EvidenceSourceType::CommittedEvent, true},
        {QStringLiteral("diary-index-1"), EvidenceSourceType::DiaryIndex, true}
    };

    const auto result = fixture.selfModel->evolve(proposal, evidence);

    QVERIFY(result.isOk());
    QVERIFY(!result.value().versionId.isEmpty());
    QCOMPARE(result.value().narrative, QStringLiteral("我和主人相处时更愿意表达。"));
}

void TestIdentityState::
evolve_whenNarrativeReferencesOnlyItself_shouldRejectCircularEvidence() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    SelfModelProposal proposal;
    proposal.narrative = QStringLiteral("我一直如此。 ");
    proposal.proposedAt = kWindowEnd;
    EvidenceSet evidence;
    evidence.items = {
        {QStringLiteral("self-version"), EvidenceSourceType::SelfModel, true}
    };

    const auto result = fixture.selfModel->evolve(proposal, evidence);

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("IDENTITY_EVIDENCE_INVALID"));
}

void TestIdentityState::
project_whenSessionSnapshotIsValid_shouldMergeBaselineRelationshipAndBoundedSlots() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    addConsolidationEvidence(*fixture.personality);
    const auto personality = fixture.personality->consolidate(consolidationRequest());
    QVERIFY(personality.isOk());
    RelationshipEvidence relationshipEvidence;
    relationshipEvidence.tendency = QStringLiteral("affinity");
    relationshipEvidence.direction = 1.0;
    relationshipEvidence.weight = 1.0;
    relationshipEvidence.confidence = 1.0;
    relationshipEvidence.sourceEventId = QStringLiteral("relationship-private-source");
    relationshipEvidence.occurredAt = kWindowEnd;
    const auto relationship = fixture.relationship->applyEvidence(
        QStringLiteral("owner"), relationshipEvidence);
    QVERIFY(relationship.isOk());
    StaticEmotionProvider emotion;
    emotion.snapshot.schemaVersion = 1;
    emotion.snapshot.value.active = EmotionType::Joy;
    emotion.snapshot.value.intensity = 0.9;
    PersonalityPolicy policy;
    policy.maxPromptSlotChars = 96;
    PersonaProjector projector(
        IdentityBaseline::defaults(), policy, &fixture.repository, &emotion);
    RuntimeSnapshot snapshot;
    snapshot.profileId = kProfileId;
    snapshot.subjectId = QStringLiteral("owner");
    snapshot.personalityVersion = personality.value().version;
    snapshot.relationshipVersion = relationship.value().version;

    const PersonaProjection projection = projector.project(
        snapshot, {QStringLiteral("Milltina"), kWindowEnd});

    QCOMPARE(projection.behaviorParameters.value(QStringLiteral("sociability")), 0.49);
    QVERIFY(!projection.promptSlots.value(QStringLiteral("speaking_style")).isEmpty());
    for (const QString& value : projection.promptSlots) {
        QVERIFY(value.size() <= policy.maxPromptSlotChars);
    }
    QVERIFY(!projection.promptSlots.values().join(QStringLiteral(" "))
                 .contains(relationshipEvidence.sourceEventId));
}

void TestIdentityState::
project_whenNullEmotionProviderUsed_shouldOmitEmotionPromptAndPrivateInternals() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    NullEmotionStateProvider emotion;
    PersonaProjector projector(
        IdentityBaseline::defaults(), PersonalityPolicy{}, &fixture.repository, &emotion);
    RuntimeSnapshot snapshot;
    snapshot.profileId = kProfileId;
    snapshot.subjectId = QStringLiteral("owner");

    const PersonaProjection projection = projector.project(
        snapshot, {QStringLiteral("Milltina"), kWindowEnd});
    const QString prompt = projection.promptSlots.values().join(QStringLiteral(" "));

    QVERIFY(!prompt.contains(QStringLiteral("情绪")));
    QVERIFY(!prompt.contains(QStringLiteral("evidence"), Qt::CaseInsensitive));
    QVERIFY(!prompt.contains(QStringLiteral("diary"), Qt::CaseInsensitive));
}

void TestIdentityState::
getPersonalityPolicy_whenConfigured_shouldReturnSanitizedLimits() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QJsonObject policy{
        {QStringLiteral("minimumIndependentEvidence"), 0},
        {QStringLiteral("minimumContextKeys"), 99},
        {QStringLiteral("minimumWindowDays"), 21},
        {QStringLiteral("maxDeltaPerWindow"), 5.0},
        {QStringLiteral("maxPromptSlotChars"), 999999}
    };
    const QJsonObject root{{QStringLiteral("aiSettings"), QJsonObject{
        {QStringLiteral("personalityPolicy"), policy}}}};
    const QString path = directory.filePath(QStringLiteral("configured.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) > 0);
    file.close();

    ConfigManager& manager = ConfigManager::instance();
    QVERIFY(manager.loadConfig(path));
    const PersonalityPolicy configured = manager.getPersonalityPolicy();
    QCOMPARE(configured.minimumIndependentEvidence, 3);
    QCOMPARE(configured.minimumContextKeys, 16);
    QCOMPARE(configured.minimumWindowDays, 21);
    QCOMPARE(configured.maxDeltaPerWindow, 0.25);
    QCOMPARE(configured.maxPromptSlotChars, 4096);
}

void TestIdentityState::
getPersonalityPolicy_whenMissing_shouldUseSafeDefaults() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("default.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(QJsonDocument(QJsonObject{
        {QStringLiteral("aiSettings"), QJsonObject{}}}).toJson(QJsonDocument::Compact)) > 0);
    file.close();

    ConfigManager& manager = ConfigManager::instance();
    QVERIFY(manager.loadConfig(path));
    const PersonalityPolicy defaults = manager.getPersonalityPolicy();
    QCOMPARE(defaults.minimumIndependentEvidence, 3);
    QCOMPARE(defaults.minimumContextKeys, 2);
    QCOMPARE(defaults.minimumWindowDays, 14);
    QCOMPARE(defaults.maxDeltaPerWindow, 0.05);
    QCOMPARE(defaults.maxPromptSlotChars, 512);
}

void TestIdentityState::
start_whenIdentityDependenciesAreValid_shouldExposeServicesAndRegisterPersonalityEvent() {
    RuntimeFixture fixture;
    QVERIFY(fixture.ready());
    QVERIFY(fixture.services.personalityService());
    QVERIFY(fixture.services.relationshipService());
    QVERIFY(fixture.services.selfModelService());
    addConsolidationEvidence(*fixture.services.personalityService());
    const auto consolidated = fixture.services.personalityService()->consolidate(
        consolidationRequest());
    QVERIFY(consolidated.isOk());

    const auto dispatched = fixture.services.eventOutbox()->dispatchPending(1);

    QVERIFY(dispatched.isOk());
    QCOMPARE(dispatched.value(), 1);
}

void TestIdentityState::
captureSnapshot_whenIdentityVersionsExist_shouldPinSubjectSpecificCommittedVersions() {
    RuntimeFixture fixture;
    QVERIFY(fixture.ready());
    addConsolidationEvidence(*fixture.services.personalityService());
    const auto personality = fixture.services.personalityService()->consolidate(
        consolidationRequest());
    QVERIFY(personality.isOk());
    RelationshipEvidence relationship;
    relationship.tendency = QStringLiteral("affinity");
    relationship.direction = 1.0;
    relationship.weight = 1.0;
    relationship.confidence = 1.0;
    relationship.sourceEventId = QStringLiteral("owner-source");
    relationship.occurredAt = kWindowEnd;
    const auto owner = fixture.services.relationshipService()->applyEvidence(
        QStringLiteral("owner"), relationship);
    QVERIFY(owner.isOk());
    SelfModelProposal proposal;
    proposal.narrative = QStringLiteral("我会基于真实经历调整表达。 ");
    proposal.proposedAt = kWindowEnd;
    EvidenceSet evidence;
    evidence.items = {{QStringLiteral("event-1"), EvidenceSourceType::CommittedEvent, true}};
    const auto selfModel = fixture.services.selfModelService()->evolve(proposal, evidence);
    QVERIFY(selfModel.isOk());

    const RuntimeSnapshot snapshot = fixture.services.captureSnapshot(
        QStringLiteral("session-1"), QStringLiteral("owner"));

    QCOMPARE(snapshot.subjectId, QStringLiteral("owner"));
    QVERIFY(snapshot.personalityVersion.has_value());
    QCOMPARE(*snapshot.personalityVersion, static_cast<qint64>(personality.value().version));
    QVERIFY(snapshot.relationshipVersion.has_value());
    QCOMPARE(*snapshot.relationshipVersion, static_cast<qint64>(owner.value().version));
    QVERIFY(snapshot.selfModelVersion.has_value());
    QCOMPARE(*snapshot.selfModelVersion, selfModel.value().versionId);
    const RuntimeSnapshot unknown = fixture.services.captureSnapshot(
        QStringLiteral("session-2"), QStringLiteral("unknown"));
    QVERIFY(!unknown.relationshipVersion.has_value());
}

void TestIdentityState::baseline_hasSevenTraitsAndBoundedEffectiveValues() {
    PersonalitySnapshot snapshot;
    QCOMPARE(snapshot.baseline.traits.size(), 7);
    QCOMPARE(snapshot.baseline.traits.value(QStringLiteral("persistence")), 0.55);
    QCOMPARE(snapshot.baseline.traits.value(QStringLiteral("caution")), 0.60);
    QCOMPARE(snapshot.baseline.traits.value(QStringLiteral("autonomy")), 0.50);
    QCOMPARE(snapshot.baseline.traits.value(QStringLiteral("helpfulness")), 0.70);
    snapshot.tendencies[QStringLiteral("helpfulness")] = 9.0;
    snapshot.baseline.traits[QStringLiteral("initiative")] = 0.0;
    snapshot.tendencies[QStringLiteral("initiative")] = -9.0;
    QCOMPARE(snapshot.effectiveTraits().value(QStringLiteral("helpfulness")), 0.95);
    QCOMPARE(snapshot.effectiveTraits().value(QStringLiteral("initiative")), 0.05);
    const RelationshipSnapshot relationship;
    QCOMPARE(relationship.tendencies.size(), 4);
    QCOMPARE(relationship.favorability(), 0.415);
}

void TestIdentityState::evidence_derivativeSourcesCannotMultiplyOriginalExperience() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    for (int index = 0; index < 3; ++index) {
        auto evidence = traitEvidence(QStringLiteral("diary-%1").arg(index),
                                      QStringLiteral("scene-%1").arg(index));
        evidence.originalSourceId = QStringLiteral("one-experience");
        evidence.sourceCategory = QStringLiteral("diary");
        evidence.category = TraitEvidenceCategory::Speculation;
        evidence.originalOccurredAt = kWindowEnd.addDays(-15);
        evidence.createdAt = kWindowEnd.addDays(-index);
        QVERIFY(fixture.personality->recordEvidence(evidence).isOk());
    }
    const auto pending = fixture.repository.pendingEvidence(kProfileId, kWindowEnd.addDays(-21), kWindowEnd);
    QVERIFY(pending.isOk());
    QCOMPARE(pending.value().size(), 1);
    QCOMPARE(pending.value().first().category, TraitEvidenceCategory::Speculation);
    QCOMPARE(pending.value().first().originalSourceId, QStringLiteral("one-experience"));
    const auto result = fixture.personality->consolidate(consolidationRequest());
    QVERIFY(result.isOk());
    QCOMPARE(result.value().version, 0);
}

void TestIdentityState::consolidate_requiresActualEvidenceSpanAndCommitCadence() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    for (int index = 0; index < 3; ++index) {
        QVERIFY(fixture.personality->recordEvidence(traitEvidence(QStringLiteral("same-day-%1").arg(index),
            QStringLiteral("scene-%1").arg(index))).isOk());
    }
    auto result = fixture.personality->consolidate(consolidationRequest());
    QVERIFY(result.isOk());
    QCOMPARE(result.value().version, 0);
    addConsolidationEvidence(*fixture.personality);
    result = fixture.personality->consolidate(consolidationRequest());
    QVERIFY(result.isOk());
    QCOMPARE(result.value().version, 1);
    addConsolidationEvidence(*fixture.personality, QStringLiteral("new-source"));
    auto request = consolidationRequest(1);
    request.windowEnd = kWindowEnd.addDays(1);
    result = fixture.personality->consolidate(request);
    QVERIFY(result.isOk());
    QCOMPARE(result.value().version, 1);
    const auto pending = fixture.repository.pendingEvidence(kProfileId, request.windowStart, request.windowEnd);
    QVERIFY(pending.isOk());
    QCOMPARE(pending.value().size(), 3);
}

void TestIdentityState::consolidate_enforcesEveryTraitCapAndPersistsCoherentSummaries() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    const auto defaults = IdentityBaseline::defaults();
    for (auto trait = defaults.traits.cbegin(); trait != defaults.traits.cend(); ++trait) {
        for (int index = 0; index < 3; ++index) {
            auto evidence = traitEvidence(QStringLiteral("experience-%1").arg(index),
                QStringLiteral("scene-%1").arg(index), 1.0, kWindowEnd.addDays(-15 + index * 7));
            evidence.trait = trait.key();
            QVERIFY(fixture.personality->recordEvidence(evidence).isOk());
        }
    }
    const auto evaluated = fixture.personality->preview(consolidationRequest());
    QVERIFY(evaluated.isOk());
    QVERIFY(evaluated.value().hasChanges);
    QCOMPARE(evaluated.value().candidate.version, 1);
    for (auto trait = defaults.traits.cbegin(); trait != defaults.traits.cend(); ++trait) {
        QCOMPARE(evaluated.value().candidate.tendencies.value(trait.key()), traitConsolidationLimit(trait.key()));
    }
    const auto result = fixture.personality->commit(evaluated.value(), QStringLiteral("最终七维参数的风格摘要"),
                                                   QStringLiteral("基于长期经历的自我叙事"));
    QVERIFY(result.isOk());
    const auto stored = fixture.repository.currentPersonality(kProfileId);
    QVERIFY(stored.isOk() && stored.value().has_value());
    QCOMPARE(stored.value()->parentVersion, 0);
    QCOMPARE(stored.value()->behaviorStyleSummary, QStringLiteral("最终七维参数的风格摘要"));
    QCOMPARE(stored.value()->selfNarrativeSummary, QStringLiteral("基于长期经历的自我叙事"));
    const auto evidence = fixture.repository.evidenceBySource(kProfileId, QStringLiteral("experience-0"));
    QVERIFY(evidence.isOk());
    QCOMPARE(evidence.value().size(), 7);
    for (const auto& item : evidence.value()) {
        QCOMPARE(item.status, TraitEvidenceStatus::Consumed);
        QCOMPARE(item.consumedVersion, 1);
        auto duplicate = static_cast<const TraitEvidenceDraft&>(item);
        duplicate.sourceEventId += QStringLiteral("-reread");
        QVERIFY(fixture.personality->recordEvidence(duplicate).isOk());
    }
    const auto pending = fixture.repository.pendingEvidence(kProfileId, kWindowEnd.addDays(-21), kWindowEnd);
    QVERIFY(pending.isOk());
    QVERIFY(pending.value().isEmpty());
}

void TestIdentityState::consolidate_respectsFrozenEventCutoff() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    for (int index = 0; index < 3; ++index) {
        auto evidence = traitEvidence(QStringLiteral("cutoff-%1").arg(index), QStringLiteral("scene-%1").arg(index),
                                      1.0, kWindowEnd.addDays(-15 + index * 7));
        evidence.sourceSequence = index + 1;
        QVERIFY(fixture.personality->recordEvidence(evidence).isOk());
    }
    auto request = consolidationRequest();
    request.evidenceCutoffSequence = 2;
    const auto result = fixture.personality->consolidate(request);
    QVERIFY(result.isOk());
    QCOMPARE(result.value().version, 0);
    auto later = static_cast<const TraitEvidenceDraft&>(fixture.repository.evidenceBySource(
        kProfileId, QStringLiteral("cutoff-2")).value().first());
    later.sourceSequence = 0;
    later.direction = -1.0;
    QVERIFY(fixture.personality->recordEvidence(later).isOk());
    QCOMPARE(fixture.repository.evidenceBySource(kProfileId, later.sourceEventId).value().first().sourceSequence, 3);
    const auto stillExcluded = fixture.personality->consolidate(request);
    QVERIFY(stillExcluded.isOk());
    QCOMPARE(stillExcluded.value().version, 0);
}

void TestIdentityState::commit_rejectsStalePreviewWithoutConsumingNewEvidence() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    addConsolidationEvidence(*fixture.personality);
    const auto evaluated = fixture.personality->preview(consolidationRequest());
    QVERIFY(evaluated.isOk());
    QVERIFY(fixture.personality->commit(evaluated.value(), {}, {}).isOk());
    addConsolidationEvidence(*fixture.personality, QStringLiteral("fresh"));
    const auto stale = fixture.personality->commit(evaluated.value(), QStringLiteral("stale"), QStringLiteral("stale"));
    QVERIFY(!stale.isOk());
    QCOMPARE(stale.error().code, QStringLiteral("STATE_VERSION_CONFLICT"));
    const auto pending = fixture.repository.pendingEvidence(kProfileId, kWindowEnd.addDays(-21), kWindowEnd);
    QVERIFY(pending.isOk());
    QCOMPARE(pending.value().size(), 3);
}

void TestIdentityState::commit_outboxFailureRollsBackStateAndConsumption() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    class FailingOutbox : public EventOutbox {
    public:
        Result<QString, DomainError> enqueue(RuntimeUnitOfWork&, const EventDraft&) override {
            return Result<QString, DomainError>::failure(domainError(QStringLiteral("INJECTED_FAILURE"), QStringLiteral("outbox failed")));
        }
        Result<int, DomainError> dispatchPending(int) override { return Result<int, DomainError>::success(0); }
    } failingOutbox;
    PersonalityService personality(kProfileId, IdentityBaseline::defaults(), {}, &fixture.repository,
                                   fixture.unitOfWorkFactory.get(), &failingOutbox);
    addConsolidationEvidence(personality);
    const auto result = personality.consolidate(consolidationRequest());
    QVERIFY(!result.isOk());
    const auto state = fixture.repository.currentPersonality(kProfileId);
    QVERIFY(state.isOk());
    QVERIFY(!state.value().has_value());
    const auto pending = fixture.repository.pendingEvidence(kProfileId, kWindowEnd.addDays(-21), kWindowEnd);
    QVERIFY(pending.isOk());
    QCOMPARE(pending.value().size(), 3);
}

void TestIdentityState::relationship_dailyAbsoluteBudgetSurvivesRestartAndReversal() {
    IdentityFixture fixture;
    QVERIFY(fixture.ready());
    RelationshipEvidence evidence;
    evidence.tendency = QStringLiteral("trust");
    evidence.direction = 1.0;
    evidence.weight = evidence.confidence = 1.0;
    evidence.occurredAt = kWindowEnd;
    evidence.sourceEventId = QStringLiteral("first");
    auto result = fixture.relationship->applyEvidence(QStringLiteral("owner"), evidence);
    QVERIFY(result.isOk());
    QCOMPARE(result.value().tendencies.value(QStringLiteral("trust")), 0.47);
    evidence.sourceEventId = QStringLiteral("second");
    QVERIFY(fixture.relationship->applyEvidence(QStringLiteral("owner"), evidence).isOk());
    fixture.repository.close();
    QVERIFY(fixture.repository.open(fixture.databasePath).isOk());
    evidence.direction = -1.0;
    evidence.sourceEventId = QStringLiteral("third");
    result = fixture.relationship->applyEvidence(QStringLiteral("owner"), evidence);
    QVERIFY(result.isOk());
    QVERIFY(qAbs(result.value().tendencies.value(QStringLiteral("trust")) - 0.48) < 1e-9);
    evidence.sourceEventId = QStringLiteral("fourth");
    result = fixture.relationship->applyEvidence(QStringLiteral("owner"), evidence);
    QVERIFY(result.isOk());
    QVERIFY(qAbs(result.value().tendencies.value(QStringLiteral("trust")) - 0.48) < 1e-9);
    evidence.occurredAt = kWindowEnd.addDays(1);
    evidence.sourceEventId = QStringLiteral("first");
    result = fixture.relationship->applyEvidence(QStringLiteral("owner"), evidence);
    QVERIFY(result.isOk());
    QVERIFY(qAbs(result.value().tendencies.value(QStringLiteral("trust")) - 0.48) < 1e-9);
    evidence.sourceEventId = QStringLiteral("decline");
    evidence.eventKind = QStringLiteral("declined");
    const auto version = result.value().version;
    result = fixture.relationship->applyEvidence(QStringLiteral("owner"), evidence);
    QVERIFY(result.isOk());
    QCOMPARE(result.value().version, version);
}

QTEST_MAIN(TestIdentityState)
#include "test_identity_state.moc"
