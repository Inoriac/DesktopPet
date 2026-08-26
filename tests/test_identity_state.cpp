#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QTest>
#include <QTemporaryDir>

#include <memory>

#include "ai/ai_brain.h"
#include "ai/event/event_outbox.h"
#include "ai/event/event_schema_registry.h"
#include "ai/event/runtime_unit_of_work.h"
#include "ai/event/sqlite_event_repository.h"
#include "ai/identity/personality_service.h"
#include "ai/identity/relationship_service.h"
#include "ai/identity/self_model_service.h"
#include "ai/identity/sqlite_identity_repository.h"
#include "ai/integration/emotion_state_provider.h"
#include "ai/runtime/agent_bootstrap.h"
#include "ai/runtime/agent_runtime_services.h"
#include "ai/runtime/runtime_ui_bridge.h"
#include "configLoader/config_manager.h"
#include "entity/pet_personality.h"

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
        std::move(callbacks),
        reinterpret_cast<AnimationPlayer*>(quintptr(1)),
        reinterpret_cast<AnimationManager*>(quintptr(2)));
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
                                                  QStringLiteral("planning"))).isOk());
    QVERIFY(service.recordEvidence(traitEvidence(prefix + QStringLiteral("-3"),
                                                  QStringLiteral("reflection"))).isOk());
}

} // namespace

class TestIdentityState : public QObject {
    Q_OBJECT

private slots:
    void recordEvidence_whenExplicitCorrectionArrives_shouldPersistTraceableWeightedEvidence();
    void recordEvidence_whenSameSourceIsRepeated_shouldDeduplicateEvidence();
    void consolidate_whenIndependentLongWindowEvidencePassesThreshold_shouldAppendLimitedStateVersion();
    void consolidate_whenOnlyNeutralEmotionExists_shouldNotMoveBaseline();
    void consolidate_whenVersionConflictsTwice_shouldLeaveEvidencePending();
    void rollback_whenTargetVersionExists_shouldAppendRestoredVersionWithoutDeletingHistory();
    void applyEvidence_whenOwnerEvidenceArrives_shouldChangeOnlyOwnerRelationship();
    void evolve_whenProposalHasCommittedIndependentEvidence_shouldAppendNarrativeVersion();
    void evolve_whenNarrativeReferencesOnlyItself_shouldRejectCircularEvidence();
    void project_whenSessionSnapshotIsValid_shouldMergeBaselineRelationshipAndBoundedSlots();
    void project_whenNullEmotionProviderUsed_shouldOmitEmotionPromptAndPrivateInternals();
    void project_whenReminderPersonalityExists_shouldNotReadOrMutateReminderSettings();
    void getPersonalityPolicy_whenConfigured_shouldReturnSanitizedLimits();
    void getPersonalityPolicy_whenMissing_shouldUseSafeDefaults();
    void start_whenIdentityDependenciesAreValid_shouldExposeServicesAndRegisterPersonalityEvent();
    void captureSnapshot_whenIdentityVersionsExist_shouldPinSubjectSpecificCommittedVersions();
    void buildBaseMessages_whenRuntimeSnapshotIsBound_shouldUseProjectedPersonaWithoutNumericEmotion();
};

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
consolidate_whenVersionConflictsTwice_shouldLeaveEvidencePending() {
    ConflictingIdentityRepository repository;
    IdentityFixture fixture(&repository);
    QVERIFY(fixture.ready());
    addConsolidationEvidence(*fixture.personality, QStringLiteral("conflict"));

    const auto result = fixture.personality->consolidate(consolidationRequest());

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("STATE_VERSION_CONFLICT"));
    QCOMPARE(repository.appendAttempts, 2);
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
    evidence.tendency = QStringLiteral("initiative");
    evidence.direction = 1.0;
    evidence.weight = 0.8;
    evidence.confidence = 1.0;
    evidence.sourceEventId = QStringLiteral("owner-event");
    evidence.occurredAt = kWindowEnd;

    const auto owner = fixture.relationship->applyEvidence(QStringLiteral("owner"), evidence);

    QVERIFY(owner.isOk());
    QCOMPARE(owner.value().version, 1);
    QVERIFY(owner.value().tendencies.value(QStringLiteral("initiative")) > 0.0);
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
    relationshipEvidence.tendency = QStringLiteral("initiative");
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

    QVERIFY(projection.promptSlots.value(QStringLiteral("persona_traits"))
                .contains(QStringLiteral("乐于交流")));
    QVERIFY(projection.promptSlots.value(QStringLiteral("persona_traits"))
                .contains(QStringLiteral("愉快")));
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
project_whenReminderPersonalityExists_shouldNotReadOrMutateReminderSettings() {
    PetPersonality reminder;
    reminder.name = QStringLiteral("reminder-only");
    reminder.forgetProbability = 0.42;
    reminder.randomVariance = 17;
    reminder.reminderPhrases = {QStringLiteral("该休息了")};
    const QJsonObject before = reminder.toJson();
    NullEmotionStateProvider emotion;
    PersonaProjector projector(
        IdentityBaseline::defaults(), PersonalityPolicy{}, nullptr, &emotion);
    RuntimeSnapshot snapshot;
    snapshot.profileId = kProfileId;
    snapshot.subjectId = QStringLiteral("owner");

    Q_UNUSED(projector.project(snapshot, {QStringLiteral("Milltina"), kWindowEnd}))
    QCOMPARE(reminder.toJson(), before);
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
    relationship.tendency = QStringLiteral("initiative");
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

void TestIdentityState::
buildBaseMessages_whenRuntimeSnapshotIsBound_shouldUseProjectedPersonaWithoutNumericEmotion() {
    RuntimeFixture fixture;
    QVERIFY(fixture.ready());
    fixture.brain.setPetName(QStringLiteral("Milltina"));
    fixture.brain.setIdentityBaseline(IdentityBaseline::defaults());
    fixture.brain.setEmotionSnapshotProvider([&fixture]() -> std::optional<EmotionSnapshot> {
        return fixture.emotion.snapshot.value;
    });

    const QString sessionId = fixture.brain.beginRuntimeSession(
        QStringLiteral("hello"), QStringLiteral("manual"));
    const QList<ChatMessage> messages = fixture.brain.buildBaseMessages(
        QStringLiteral("hello"), QStringLiteral("manual"), sessionId);

    QVERIFY(!sessionId.isEmpty());
    QVERIFY(!messages.isEmpty());
    const QString prompt = messages.first().content + QStringLiteral("\n")
        + messages.last().content;
    QVERIFY(prompt.contains(QStringLiteral("愉快")));
    QVERIFY(!prompt.contains(QStringLiteral("mood_valence")));
    QVERIFY(!prompt.contains(QStringLiteral("emotion_intensity")));
    QVERIFY(!prompt.contains(QStringLiteral("0.80")));
}

QTEST_MAIN(TestIdentityState)
#include "test_identity_state.moc"
