#include <QtTest>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "ai/context/context_assembler.h"
#include "ai/event/event_outbox.h"
#include "ai/event/event_schema_registry.h"
#include "ai/event/runtime_unit_of_work.h"
#include "ai/event/sqlite_event_repository.h"
#include "ai/identity/personality_service.h"
#include "ai/identity/sqlite_identity_repository.h"
#include "ai/model/model_role_registry.h"
#include "ai/model/model_router.h"
#include "ai/reflection/personality_consolidation_stage.h"

namespace {
const QString profile = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-11T08:00:00Z"), Qt::ISODate);

class DeferredModel final : public ModelCompletionClient {
public:
    QList<QList<ChatMessage>> requests;
    QList<LlmCompletionHandler> pending;
    void completeOnce(const ModelRouteConfig&, const QList<ChatMessage>& messages,
                      const QJsonArray&, LlmCompletionHandler callback,
                      const QString&) override {
        requests.append(messages);
        pending.append(std::move(callback));
    }
    void reply(const QString& text, bool success = true) {
        auto callback = pending.takeFirst();
        LlmResponse response;
        response.content = text;
        callback(success, response, success ? QString{} : QStringLiteral("provider unavailable"));
    }
};

ModelRoleConfig evolutionRole() {
    ModelRouteConfig route;
    route.routeId = QStringLiteral("remote-evolution");
    route.enabled = true;
    route.llm.enabled = true;
    route.llm.provider = QStringLiteral("configured-provider");
    route.llm.baseUrl = QStringLiteral("https://example.invalid/v1");
    route.llm.apiKey = QStringLiteral("test");
    route.llm.model = QStringLiteral("model");
    ModelRoleConfig role;
    role.role = ModelRole::PersonalityEvolution;
    role.routes = {route};
    return role;
}

QList<EventRecord> events() {
    QList<EventRecord> result;
    for (int i = 0; i < 3; ++i) {
        EventRecord event;
        event.profileId = profile;
        event.eventId = QStringLiteral("original-%1").arg(i + 1);
        event.sequence = i + 1;
        event.occurredAt = now.addDays(i == 0 ? -20 : i == 1 ? -10 : -1);
        event.type = i == 1 ? QStringLiteral("TaskExperienceCommitted")
                            : QStringLiteral("UserMessageReceived");
        event.payload = {{QStringLiteral("text"), QStringLiteral("a shared experience")}};
        result.append(event);
    }
    return result;
}

QString proposal(bool reflection = false) {
    QJsonArray evidence;
    for (int i = 1; i <= 3; ++i) {
        const QString source = QStringLiteral("original-%1").arg(i);
        evidence.append(QJsonObject{
            {QStringLiteral("materialId"), reflection && i == 3 ? QStringLiteral("old-diary") : source},
            {QStringLiteral("originalSourceId"), source},
            {QStringLiteral("trait"), QStringLiteral("openness")},
            {QStringLiteral("scene"), i == 2 ? QStringLiteral("problem_solving") : QStringLiteral("companionship")},
            {QStringLiteral("direction"), 1.0},
            {QStringLiteral("weight"), 1.0},
            {QStringLiteral("confidence"), 1.0},
            {QStringLiteral("category"), reflection && i == 3 ? QStringLiteral("feeling") : QStringLiteral("fact")}});
    }
    return QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("evidence"), evidence}}).toJson(QJsonDocument::Compact));
}

QString summary(bool correct = true) {
    auto effective = IdentityBaseline::defaults().traits;
    effective[QStringLiteral("openness")] += correct ? 0.05 : 0.30;
    return QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("effectiveTraits"), numericMapToJson(effective)},
        {QStringLiteral("behaviorStyleSummary"), QStringLiteral("对新问题更愿意探索，也尊重边界。")},
        {QStringLiteral("selfNarrativeSummary"), QStringLiteral("共同经历让我更愿意了解陌生事物。")}
    }).toJson(QJsonDocument::Compact));
}

struct Fixture {
    QTemporaryDir directory;
    QString path = directory.filePath(QStringLiteral("identity.sqlite"));
    SqliteEventRepository eventRepository;
    SqliteIdentityRepository identity;
    EventSchemaRegistry schemas;
    std::unique_ptr<SqliteRuntimeUnitOfWorkFactory> uow;
    std::unique_ptr<SqliteEventOutbox> outbox;
    std::unique_ptr<PersonalityService> personality;
    DeferredModel model;
    ModelRoleRegistry registry{{evolutionRole()}};
    ModelRouter router{&registry, &model};
    ContextAssembler assembler;
    PersonalityEvolutionHooks hooks;
    QList<EventRecord> committedEvents = events();
    std::unique_ptr<PersonalityConsolidationStage> stage;
    CancellationSource cancellation;
    std::optional<Result<PersonalitySnapshot, DomainError>> result;

    bool open() {
        if (!eventRepository.open(path).isOk() || !identity.open(path).isOk()
            || !registerBuiltInEventSchemas(schemas).isOk()) return false;
        schemas.freeze();
        uow = std::make_unique<SqliteRuntimeUnitOfWorkFactory>(path);
        outbox = std::make_unique<SqliteEventOutbox>(path, &schemas, profile);
        personality = std::make_unique<PersonalityService>(profile, IdentityBaseline::defaults(),
            PersonalityPolicy{}, &identity, uow.get(), outbox.get());
        hooks.readCommittedEvents = [this](qint64) {
            return Result<QList<EventRecord>, DomainError>::success(committedEvents);
        };
        return true;
    }
    void run(qint64 cutoff = 3, QDateTime at = now) {
        result.reset();
        stage = std::make_unique<PersonalityConsolidationStage>(profile,
            personality.get(), &identity, &router, &assembler, hooks);
        stage->consolidateAsync({profile, QStringLiteral("sleep-cycle"), cutoff, at},
            cancellation.token(), [this](auto value) { result = std::move(value); });
    }
};
} // namespace

class PersonalityConsolidationTests : public QObject {
    Q_OBJECT
private slots:
    void lateExtractionCannotWriteAfterCancellation();
    void lateSummaryCannotCommitAfterCancellation();
    void providerFailureKeepsVersionAndEvidence();
    void invalidSourceCannotBecomeEvidence();
    void invalidSummaryFallsBackToFinalParameters();
    void privateDiaryUsesFrozenProvenanceAndCannotGrowTwice();
    void legacyDiaryCannotInventSameDayOriginals();
    void reflectionAcrossConversationScenesCanContributeWithoutFeedback();
    void staleSummaryTriggersFreshExtractionAgainstNewVersion();
    void taskRequestAndCommandsDoNotBecomeIndependentOriginals();
    void revisedProposalReplacesPendingEvidenceAfterCancelledSummary();
    void duplicateOriginalAcrossMaterialsRejectsBatchBeforeRevision();
};

void PersonalityConsolidationTests::revisedProposalReplacesPendingEvidenceAfterCancelledSummary() {
    Fixture f;
    QVERIFY(f.open());
    f.run();
    f.model.reply(proposal());
    const auto initial = f.identity.pendingEvidence(profile, now.addDays(-30), now).value();
    QCOMPARE(initial.size(), 3);
    f.cancellation.cancel();
    f.model.reply(summary());
    QVERIFY(f.result.has_value() && !f.result->isOk());
    f.cancellation.reset();
    f.run();
    auto response = QJsonDocument::fromJson(proposal().toUtf8()).object();
    auto rows = response.value(QStringLiteral("evidence")).toArray();
    for (int i = 0; i < rows.size(); ++i) {
        auto row = rows[i].toObject();
        row.insert(QStringLiteral("direction"), -1.0);
        rows[i] = row;
    }
    response.insert(QStringLiteral("evidence"), rows);
    f.model.reply(QString::fromUtf8(QJsonDocument(response).toJson()));
    const auto revised = f.identity.pendingEvidence(profile, now.addDays(-30), now).value();
    QCOMPARE(revised.size(), 3);
    for (int i = 0; i < revised.size(); ++i) {
        QCOMPARE(revised[i].evidenceId, initial[i].evidenceId);
        QCOMPARE(revised[i].originalOccurredAt, initial[i].originalOccurredAt);
        QCOMPARE(revised[i].direction, -1.0);
        QCOMPARE(revised[i].revision, 2);
    }
    f.model.reply(summary(false)); // Mismatched echo uses the validated deterministic fallback.
    QVERIFY(f.result.has_value() && f.result->isOk());
    QCOMPARE(f.result->value().effectiveTraits().value(QStringLiteral("openness")), 0.55);
}

void PersonalityConsolidationTests::duplicateOriginalAcrossMaterialsRejectsBatchBeforeRevision() {
    Fixture f;
    QVERIFY(f.open());
    TraitEvidenceDraft original;
    original.trait = QStringLiteral("openness");
    original.direction = 0.75;
    original.weight = original.confidence = 1.0;
    original.contextKey = QStringLiteral("companionship");
    original.sourceEventId = original.originalSourceId = QStringLiteral("original-1");
    original.createdAt = original.originalOccurredAt = f.committedEvents.first().occurredAt;
    original.sourceSequence = 1;
    QVERIFY(f.personality->recordEvidence(original).isOk());
    EventRecord thought;
    thought.profileId = profile;
    thought.eventId = QStringLiteral("thought-event");
    thought.type = QStringLiteral("InnerThoughtStored");
    thought.sequence = 4;
    thought.occurredAt = now.addSecs(-1);
    thought.privateReference = PrivateEventReference{QStringLiteral("private_psyche"),
        QStringLiteral("inner_thought"), QStringLiteral("same-source-thought"), profile};
    f.committedEvents.append(thought);
    f.hooks.readInnerThought = [](const QString& id) {
        InnerThoughtSummary summary;
        summary.entryId = id;
        summary.profileId = profile;
        summary.sourceEventId = QStringLiteral("original-1");
        summary.appraisal = QStringLiteral("同一个经历的新理解");
        return Result<InnerThoughtSummary, DomainError>::success(summary);
    };
    f.run(4);
    auto response = QJsonDocument::fromJson(proposal().toUtf8()).object();
    auto rows = response.value(QStringLiteral("evidence")).toArray();
    auto changed = rows.first().toObject();
    changed.insert(QStringLiteral("direction"), -1.0);
    rows[0] = changed;
    changed.insert(QStringLiteral("materialId"), QStringLiteral("same-source-thought"));
    changed.insert(QStringLiteral("category"), QStringLiteral("feeling"));
    rows.append(changed);
    response.insert(QStringLiteral("evidence"), rows);
    f.model.reply(QString::fromUtf8(QJsonDocument(response).toJson()));
    QVERIFY(f.result.has_value() && !f.result->isOk());
    QCOMPARE(f.result->error().code, QStringLiteral("MODEL_OUTPUT_INVALID"));
    const auto pending = f.identity.pendingEvidence(profile, now.addDays(-30), now).value();
    QCOMPARE(pending.size(), 1);
    QCOMPARE(pending.first().direction, 0.75);
    QCOMPARE(pending.first().revision, 1);
}

void PersonalityConsolidationTests::taskRequestAndCommandsDoNotBecomeIndependentOriginals() {
    Fixture f;
    QVERIFY(f.open());
    f.committedEvents[0].payload.insert(QStringLiteral("task_related"), true);
    f.committedEvents[2].payload.insert(QStringLiteral("task_related"), true);
    f.run();
    QCOMPARE(f.model.requests.size(), 1);
    const auto input = QJsonDocument::fromJson(f.model.requests.first().last().content.toUtf8()).object();
    const auto materials = input.value(QStringLiteral("materials")).toArray();
    QCOMPARE(materials.size(), 1);
    QCOMPARE(materials.first().toObject().value(QStringLiteral("materialId")).toString(), QStringLiteral("original-2"));

    // A valid proposal can cite only the one complete task experience.
    const auto allEvidence = QJsonDocument::fromJson(proposal().toUtf8()).object().value(QStringLiteral("evidence")).toArray();
    f.model.reply(QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("evidence"), QJsonArray{allEvidence.at(1)}}}).toJson()));
    QVERIFY(f.result.has_value() && f.result->isOk());
    QCOMPARE(f.result->value().version, 0);
    QCOMPARE(f.identity.pendingEvidence(profile, now.addDays(-30), now).value().size(), 1);

    // A model cannot restore excluded command IDs by naming them in output.
    f.run();
    f.model.reply(proposal());
    QVERIFY(f.result.has_value() && !f.result->isOk());
    QCOMPARE(f.result->error().code, QStringLiteral("MODEL_OUTPUT_INVALID"));
    QCOMPARE(f.identity.pendingEvidence(profile, now.addDays(-30), now).value().size(), 1);
}

void PersonalityConsolidationTests::lateExtractionCannotWriteAfterCancellation() {
    Fixture f;
    QVERIFY(f.open());
    f.run();
    QCOMPARE(f.model.pending.size(), 1);
    f.cancellation.cancel();
    f.model.reply(proposal());
    QVERIFY(f.result.has_value() && !f.result->isOk());
    QCOMPARE(f.result->error().code, QStringLiteral("SLEEP_CANCELLED"));
    QVERIFY(!f.identity.currentPersonality(profile).value().has_value());
    QVERIFY(f.identity.pendingEvidence(profile, now.addDays(-30), now).value().isEmpty());
}

void PersonalityConsolidationTests::lateSummaryCannotCommitAfterCancellation() {
    Fixture f;
    QVERIFY(f.open());
    f.run();
    f.model.reply(proposal());
    QCOMPARE(f.model.pending.size(), 1);
    f.cancellation.cancel();
    f.model.reply(summary());
    QVERIFY(f.result.has_value() && !f.result->isOk());
    QVERIFY(!f.identity.currentPersonality(profile).value().has_value());
    QCOMPARE(f.identity.pendingEvidence(profile, now.addDays(-30), now).value().size(), 3);
}

void PersonalityConsolidationTests::providerFailureKeepsVersionAndEvidence() {
    Fixture f;
    QVERIFY(f.open());
    f.run();
    f.model.reply({}, false);
    QVERIFY(f.result.has_value() && !f.result->isOk());
    QVERIFY(!f.identity.currentPersonality(profile).value().has_value());
    QVERIFY(f.identity.pendingEvidence(profile, now.addDays(-30), now).value().isEmpty());
}

void PersonalityConsolidationTests::invalidSourceCannotBecomeEvidence() {
    Fixture f;
    QVERIFY(f.open());
    f.run();
    QString invalid = proposal();
    invalid.replace(QStringLiteral("original-3"), QStringLiteral("invented-event"));
    f.model.reply(invalid);
    QVERIFY(f.result.has_value() && !f.result->isOk());
    QCOMPARE(f.result->error().code, QStringLiteral("MODEL_OUTPUT_INVALID"));
    QVERIFY(f.identity.pendingEvidence(profile, now.addDays(-30), now).value().isEmpty());
}

void PersonalityConsolidationTests::invalidSummaryFallsBackToFinalParameters() {
    Fixture f;
    QVERIFY(f.open());
    f.run();
    f.model.reply(proposal());
    f.model.reply(summary(false));
    QVERIFY(f.result.has_value() && f.result->isOk());
    QCOMPARE(f.result->value().version, 1);
    QCOMPARE(f.result->value().effectiveTraits().value(QStringLiteral("openness")), 0.65);
    QCOMPARE(f.result->value().behaviorStyleSummary,
             PersonalityService::deterministicBehaviorSummary(f.result->value()));
    QVERIFY(f.identity.pendingEvidence(profile, now.addDays(-30), now).value().isEmpty());
}

void PersonalityConsolidationTests::privateDiaryUsesFrozenProvenanceAndCannotGrowTwice() {
    Fixture f;
    QVERIFY(f.open());
    EventRecord diaryEvent;
    diaryEvent.profileId = profile;
    diaryEvent.eventId = QStringLiteral("diary-event");
    diaryEvent.type = QStringLiteral("DiaryStored");
    diaryEvent.sequence = 4;
    diaryEvent.occurredAt = now.addSecs(-60);
    diaryEvent.privateReference = PrivateEventReference{
        QStringLiteral("private_psyche"), QStringLiteral("diary_entry"), QStringLiteral("old-diary"), profile};
    f.committedEvents.append(diaryEvent);
    auto later = f.committedEvents.first();
    later.eventId = QStringLiteral("after-cutoff-must-not-appear");
    later.sequence = 5;
    f.committedEvents.append(later);
    const QString privateBody = QStringLiteral("私密正文：我担心被误解，但这只是我的感受。全文末尾。");
    f.hooks.readDiary = [privateBody](const QString&) {
        DiaryEntry entry;
        entry.entryId = QStringLiteral("old-diary");
        entry.profileId = profile;
        entry.localDate = now.addDays(-1).toLocalTime().date();
        entry.body = privateBody;
        entry.sourceCutoffSequence = 3;
        entry.index = {{QStringLiteral("originalSourceIds"), QJsonArray{QStringLiteral("original-3")}}};
        return Result<DiaryEntry, DomainError>::success(entry);
    };
    f.run(4);
    const auto request = f.model.requests.first().last().content;
    QVERIFY(request.contains(privateBody));
    QVERIFY(!request.contains(QStringLiteral("after-cutoff-must-not-appear")));
    f.model.reply(proposal(true));
    f.model.reply(summary());
    QVERIFY(f.result.has_value() && f.result->isOk());
    const auto evidence = f.identity.evidenceBySource(profile, QStringLiteral("old-diary"));
    QVERIFY(evidence.isOk());
    QCOMPARE(evidence.value().size(), 1);
    QCOMPARE(evidence.value().first().originalSourceId, QStringLiteral("original-3"));
    QCOMPARE(evidence.value().first().category, TraitEvidenceCategory::Feeling);
    f.run(4, now.addDays(15));
    f.model.reply(proposal(true));
    QVERIFY(f.result.has_value() && f.result->isOk());
    QCOMPARE(f.result->value().version, 1);
    QVERIFY(f.model.pending.isEmpty());
    QCOMPARE(f.identity.personalityHistory(profile).value().size(), 1);
}

void PersonalityConsolidationTests::staleSummaryTriggersFreshExtractionAgainstNewVersion() {
    Fixture f;
    QVERIFY(f.open());
    f.run();
    f.model.reply(proposal());
    PersonalityConsolidationRequest request;
    request.profileId = profile;
    request.windowStart = now.addDays(-30);
    request.windowEnd = now;
    request.evidenceCutoffSequence = 3;
    const auto concurrent = f.personality->consolidate(request);
    QVERIFY(concurrent.isOk());
    QCOMPARE(concurrent.value().version, 1);
    f.model.reply(summary());
    QCOMPARE(f.model.pending.size(), 1);
    const auto input = QJsonDocument::fromJson(f.model.requests.last().last().content.toUtf8()).object();
    QCOMPARE(input.value(QStringLiteral("currentVersion")).toInt(), 1);
    f.model.reply(QStringLiteral("{\"evidence\":[]}"));
    QVERIFY(f.result.has_value() && f.result->isOk());
    QCOMPARE(f.result->value().version, 1);
    QCOMPARE(f.identity.personalityHistory(profile).value().size(), 1);
}

void PersonalityConsolidationTests::legacyDiaryCannotInventSameDayOriginals() {
    Fixture f;
    QVERIFY(f.open());
    EventRecord diary;
    diary.profileId = profile;
    diary.eventId = QStringLiteral("legacy-diary-event");
    diary.type = QStringLiteral("DiaryStored");
    diary.sequence = 4;
    diary.occurredAt = now.addSecs(-60);
    diary.privateReference = PrivateEventReference{QStringLiteral("private_psyche"),
        QStringLiteral("diary_entry"), QStringLiteral("old-diary"), profile};
    f.committedEvents.append(diary);
    f.hooks.readDiary = [](const QString&) {
        DiaryEntry entry;
        entry.profileId = profile;
        entry.localDate = now.addDays(-1).toLocalTime().date();
        entry.body = QStringLiteral("没有来源引用的旧日记仍可阅读。");
        entry.sourceCutoffSequence = 3;
        return Result<DiaryEntry, DomainError>::success(entry);
    };
    f.run(4);
    QVERIFY(f.model.requests.first().last().content.contains(QStringLiteral("没有来源引用的旧日记")));
    f.model.reply(proposal(true));
    QVERIFY(f.result.has_value() && !f.result->isOk());
    QCOMPARE(f.result->error().code, QStringLiteral("MODEL_OUTPUT_INVALID"));
    QVERIFY(f.identity.pendingEvidence(profile, now.addDays(-30), now).value().isEmpty());
}

void PersonalityConsolidationTests::reflectionAcrossConversationScenesCanContributeWithoutFeedback() {
    Fixture f;
    QVERIFY(f.open());
    for (auto& original : f.committedEvents) original.type = QStringLiteral("UserMessageReceived");
    for (int i = 1; i <= 3; ++i) {
        EventRecord thought;
        thought.profileId = profile;
        thought.eventId = QStringLiteral("thought-event-%1").arg(i);
        thought.type = QStringLiteral("InnerThoughtStored");
        thought.sequence = i + 3;
        thought.occurredAt = now.addSecs(-60);
        thought.privateReference = PrivateEventReference{QStringLiteral("private_psyche"),
            QStringLiteral("inner_thought"), QStringLiteral("thought-%1").arg(i), profile};
        f.committedEvents.append(thought);
    }
    f.hooks.readInnerThought = [](const QString& id) {
        InnerThoughtSummary thought;
        thought.profileId = profile;
        thought.entryId = id;
        thought.sourceEventId = QStringLiteral("original-%1").arg(id.section('-', -1));
        thought.appraisal = QStringLiteral("我觉得探索新问题让我有所收获。");
        return Result<InnerThoughtSummary, DomainError>::success(thought);
    };
    auto output = QJsonDocument::fromJson(proposal().toUtf8()).object();
    auto evidence = output.value(QStringLiteral("evidence")).toArray();
    for (int i = 0; i < evidence.size(); ++i) {
        auto item = evidence.at(i).toObject();
        item.insert(QStringLiteral("materialId"), QStringLiteral("thought-%1").arg(i + 1));
        item.insert(QStringLiteral("category"), QStringLiteral("feeling"));
        evidence.replace(i, item);
    }
    output.insert(QStringLiteral("evidence"), evidence);
    f.run(6);
    f.model.reply(QString::fromUtf8(QJsonDocument(output).toJson()));
    f.model.reply(summary());
    QVERIFY(f.result.has_value() && f.result->isOk());
    QCOMPARE(f.result->value().version, 1);
    QCOMPARE(f.result->value().sourceEvidenceIds.size(), 3);
}

QTEST_GUILESS_MAIN(PersonalityConsolidationTests)
#include "test_personality_consolidation.moc"
