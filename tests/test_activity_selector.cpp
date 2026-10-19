#include "ai/activity/activity_selector.h"
#include "ai/activity/activity_store.h"
#include "ai/activity/opportunity_store.h"
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QSqlDatabase>
#include "ai/event/runtime_unit_of_work.h"
#include <QtTest>
#include "ai/memory/memory_store.h"
#include "ai/activity/activity_runtime.h"
#include "ai/chat/conversation_store.h"
#include "ai/event/event_outbox.h"
#include "ai/task/task_orchestrator.h"
#include "ai/task/task_store.h"
#include "ai/plugin/capability_discovery.h"
#include <QSignalSpy>

namespace {
const QString profile = "11111111-1111-4111-8111-111111111111";
SelectionRequest request() {
    SelectionRequest r;
    r.context.profileId = profile;
    r.context.allowInternalMemory = true;
    r.ticket.profileId = profile;
    r.ticket.ticketId = "ticket";
    r.catalogRevision = 3;
    r.cues.append({"memory:exam",
                   "memory",
                   "Friend has an exam",
                   {"event:origin", "person:friend"}});
    r.references = {"memory:exam", "event:origin", "person:friend"};
    r.capabilities.append(QJsonObject{{"pluginId", "reading.package"},
                                      {"id", "reader"},
                                      {"version", "1"},
                                      {"kind", "tool"}});
    return r;
}
QJsonObject proposal() {
    return {{"decision", "start"},
            {"reason", "Check the exam preparation"},
            {"evidenceRefs",
             QJsonArray{"memory:exam", "event:origin", "person:friend"}},
            {"goal", "Read the available update"},
            {"stopWhen", "Update checked"},
            {"reconsiderWhen", "Important reply arrives"},
            {"capabilityRefs",
             QJsonArray{QJsonObject{{"pluginId", "reading.package"},
                                    {"entryId", "reader"},
                                    {"entryVersion", "1"},
                                    {"operationId", ""}}}},
            {"candidates", QJsonArray{}},
            {"activityId", ""},
            {"nextConsiderAt", ""}};
}
struct SelectionFixture {
    QTemporaryDir dir;
    QString path = dir.filePath("runtime.db");
    ActivityStore activities{profile, path};
    OpportunityStore opportunities{profile, path};
    AutonomyConfig config;
    quint64 catalog = 3;
    SelectionFixture() {
        config.enabled = true;
        config.minDecisionIntervalMs = 1;
        if (!activities.initialize() ||
            !opportunities.configure(config, OpportunityStore::systemClock()))
            qFatal("fixture failed");
        activities.setCapabilityValidator([this](const auto &, const auto &,
                                                 bool autonomous,
                                                 quint64 version) {
            return autonomous && (!version || version == catalog)
                       ? Result<void, DomainError>::success()
                       : Result<void, DomainError>::failure(
                             domainError("STALE_REVISION", "Catalog changed"));
        });
    }
    OpportunitySignal signal() {
        OpportunitySignal s;
        s.clock = OpportunityStore::systemClock();
        s.readiness.running = true;
        s.readiness.autonomyEnabled = true;
        s.readiness.hostRevision = 1;
        return s;
    }
    SelectionRequest offered() {
        auto r = request();
        r.cues.clear();
        r.references.clear();
        r.runtimeDatabasePath = path;
        auto ticket = opportunities.considerOpportunity(signal());
        if (!ticket)
            qFatal("ticket failed: %s", qPrintable(ticket.error().message));
        r.ticket = ticket.value();
        return r;
    }
    SelectionProposal start(const SelectionRequest &r) {
        auto j = proposal();
        j["evidenceRefs"] = QJsonArray{};
        return ActivitySelector::parse(j, r).value();
    }
};
class SelectionClient : public ModelCompletionClient {
  public:
    int calls = 0, mode = 0;
    void completeOnce(const ModelRouteConfig &, const QList<ChatMessage> &,
                      const QJsonArray &, LlmCompletionHandler done,
                      const QString &) override {
        ++calls;
        if (mode == 2 || (mode == 3 && calls == 1)) {
            done(false, {}, "provider unavailable");
            return;
        }
        LlmResponse response;
        response.usage.totalTokens = 20;
        auto j = proposal();
        j["decision"] = "wait";
        j["capabilityRefs"] = QJsonArray{};
        j["evidenceRefs"] = QJsonArray{};
        response.content = (mode == 1 && calls == 1) || mode == 3
                               ? QString("invalid")
                               : QString::fromUtf8(QJsonDocument(j).toJson(
                                     QJsonDocument::Compact));
        done(true, response, {});
    }
};
QList<ModelRoleConfig> selectionRoles() {
    ModelRouteConfig route;
    route.routeId = "first";
    route.enabled = route.llm.enabled = true;
    route.llm.provider = "fake";
    route.llm.model = "fake";
    route.llm.apiKey = "test";
    route.llm.baseUrl = "https://test.invalid";
    route.llm.maxTokens = 1024;
    auto second = route;
    second.routeId = "second";
    ModelRoleConfig config;
    config.role = ModelRole::AgencyDecision;
    config.routes = {route, second};
    return {config};
}

} // namespace
class ActivitySelectorTests : public QObject {
    Q_OBJECT
  private slots:
    void capabilityCandidates_relatedTailAndExplorationCoverEveryEntry() {
        QJsonArray entries;
        for (int i = 0; i < 40; ++i)
            entries.append(QJsonObject{{"pluginId", "package"},
                {"id", QString("entry.%1").arg(i, 2, 10, QChar('0'))},
                {"name", i >= 28 ? "音乐播放器" : QString(200, 'x')},
                {"summary", i >= 28 ? "MUSIC playlists" : QString(500, 'x')},
                {"version", "1"}, {"readiness", "ready"}});
        auto disabled = entries[0].toObject();
        disabled["id"] = "disabled";
        disabled["readiness"] = "disabled";
        entries.append(disabled);
        for (const auto& query : {QString("music"), QString("现在想听点音乐"), QString("no match")}) {
            QSet<QString> seen;
            for (quint64 offset = 0; offset < 40; offset += 8) {
                const auto selected = selectCapabilityCandidates(entries, query, offset);
                QCOMPARE(selected.size(), 16);
                QSet<QString> page;
                for (int i = 0; i < selected.size(); ++i) {
                    const auto entry = selected[i].toObject();
                    const auto id = entry["id"].toString();
                    QVERIFY(id != "disabled");
                    QVERIFY(!page.contains(id));
                    page.insert(id);
                    seen.insert(id);
                    QVERIFY(entry["name"].toString().size() <= 128);
                    QVERIFY(entry["summary"].toString().size() <= 384);
                    if (query != "no match" && i < 8)
                        QVERIFY(id >= "entry.28");
                }
            }
            QCOMPARE(seen.size(), 40);
        }
        const auto before = entries;
        QCOMPARE(selectCapabilityCandidates(entries, "music", 0, 3).size(), 3);
        QVERIFY(selectCapabilityCandidates({}, {}, 0).isEmpty());
        QCOMPARE(entries, before);
    }
    void selectionMaterials_explicitMemoryAccessHonorsBoundConversation() {
        SelectionFixture f;
        ConversationStore conversations(profile, f.path);
        QVERIFY(conversations.initialize());
        HostConversationBinding binding;
        binding.channel = "sample";
        binding.connectionKey = "bound";
        binding.remoteConversationId = "room";
        QVERIFY(conversations.registerBinding(binding));
        IncomingMessage incoming;
        incoming.channel = binding.channel;
        incoming.connectionKey = binding.connectionKey;
        incoming.remoteConversationId = binding.remoteConversationId;
        incoming.remoteMessageId = "real-message";
        incoming.speakerIdentity = {"sample", "bound", "friend"};
        incoming.content = "A shared update";
        incoming.occurredAt = QDateTime::currentDateTimeUtc();
        const auto accepted = conversations.acceptMessage(incoming);
        QVERIFY(accepted);
        QVERIFY(conversations.recoverOutbox());

        const auto memoryPath = f.dir.filePath("memory.db");
        MemoryStore memories;
        memories.setProfileId(profile);
        memories.setDatabasePath(memoryPath);
        QVERIFY(memories.loadDatabaseOnly());
        QStringList ids;
        for (const auto audience :
             {MemoryAudienceKind::OwnerOnly, MemoryAudienceKind::Conversation,
              MemoryAudienceKind::Internal}) {
            MemoryEntry memory;
            memory.type = MemoryType::Semantic;
            memory.summary = QString::number(int(audience)) + " actual memory";
            memory.attribution.profileId = profile;
            memory.attribution.resolved = true;
            memory.attribution.audience.kind = audience;
            if (audience == MemoryAudienceKind::Conversation)
                memory.attribution.audience.conversationIds = {
                    accepted.value().conversationId};
            const auto person =
                accepted.value().context.participantPersonIds.first();
            memory.attribution.people = {{person, MemoryPersonRole::Subject}};
            memory.attribution.origins = {
                {accepted.value().originId, accepted.value().conversationId,
                 person, incoming.occurredAt, accepted.value().originId, true}};
            const auto saved = memories.addEntry(memory);
            QVERIFY(!saved.id.isEmpty());
            ids << "memory:" + saved.id;
        }
        auto r = request();
        r.cues.clear();
        r.runtimeDatabasePath = f.path;
        r.memoryDatabasePath = memoryPath;
        auto prepared = ActivitySelector::prepare(r);
        QVERIFY(prepared);
        QVERIFY(!prepared.value().references.contains(ids[0]));
        QVERIFY(!prepared.value().references.contains(ids[1]));
        QVERIFY(prepared.value().references.contains(ids[2]));

        r.context.allowOwnerMemory = true;
        r.context.authorizedConversationIds = {accepted.value().conversationId};
        prepared = ActivitySelector::prepare(r);
        QVERIFY(prepared);
        for (const auto &id : ids)
            QVERIFY(prepared.value().references.contains(id));
        QVERIFY(prepared.value().context.participantPersonIds.isEmpty());
        QVERIFY(prepared.value().references.contains(
            "person:" + accepted.value().context.participantPersonIds.first()));

        r.ticket.contextId = accepted.value().conversationId;
        prepared = ActivitySelector::prepare(r);
        QVERIFY(prepared);
        QCOMPARE(prepared.value().context.originKind, OriginKind::External);
        QVERIFY(!prepared.value().context.allowOwnerMemory);
        QVERIFY(!prepared.value().context.allowInternalMemory);
        QVERIFY(!prepared.value().references.contains(ids[0]));
        QVERIFY(prepared.value().references.contains(ids[1]));
        QVERIFY(!prepared.value().references.contains(ids[2]));
    }
    void
    selectionMaterials_sameScopeOldIntentVisibleAndRevokedMemoryBlocksResume() {
        auto r = request();
        ActivitySnapshot prior;
        prior.activityId = "old-reading";
        prior.profileId = profile;
        prior.context = r.context;
        prior.context.activityId = prior.activityId;
        prior.intent = {{"origin", "self"}, {"goal", "Read chapter twelve"}};
        prior.state = ActivityState::Paused;
        prior.pauseReason = "checkpoint";
        r.activities = {prior};
        QCOMPARE(ActivitySelector::materials(r)["activities"]
                     .toArray()
                     .first()
                     .toObject()["goal"]
                     .toString(),
                 QString("Read chapter twelve"));
        r.activities[0].context.allowOwnerMemory = true;
        QVERIFY(ActivitySelector::materials(r)["activities"]
                    .toArray()
                    .first()
                    .toObject()["goal"] != "Read chapter twelve");
        SelectionFixture f;
        f.config.memoryAccessOwner = true;
        QVERIFY(f.opportunities.configure(f.config,
                                          OpportunityStore::systemClock()));
        ActivityIntent intent;
        intent.context.profileId = profile;
        intent.context.allowOwnerMemory = true;
        intent.context.allowInternalMemory = true;
        intent.origin = "self";
        intent.goal = "Private reading";
        auto a = f.activities.startActivity(intent);
        QVERIFY(a);
        ActivityTransition t;
        t.profileId = profile;
        t.activityId = a.value().activityId;
        t.expectedRevision = a.value().revision;
        t.targetState = ActivityState::Paused;
        t.reason = "checkpoint";
        auto paused = f.activities.transitionActivity(t);
        QVERIFY(paused);
        f.config.memoryAccessOwner = false;
        QVERIFY(f.opportunities.configure(f.config,
                                          OpportunityStore::systemClock()));
        r = f.offered();
        r.activities = {paused.value()};
        auto j = proposal();
        j["decision"] = "resume";
        j["activityId"] = paused.value().activityId;
        j["capabilityRefs"] = QJsonArray{};
        j["evidenceRefs"] = QJsonArray{};
        auto proposed = ActivitySelector::parse(j, r);
        QVERIFY(proposed);
        auto denied = f.activities.applySelection({r, proposed.value()});
        QVERIFY(!denied);
        QCOMPARE(denied.error().code, QString("PERMISSION_DENIED"));
        QCOMPARE(f.activities.find(paused.value().activityId).value().state,
                 ActivityState::Paused);
    }
    void selectionMaterials_eligibleIntentSurvivesRecentCompletedActivities() {
        SelectionFixture f;
        ActivityIntent intent;
        intent.context.profileId = profile;
        intent.context.allowInternalMemory = true;
        intent.origin = "self";
        intent.goal = "Continue older book";
        auto a = f.activities.startActivity(intent);
        QVERIFY(a);
        ActivityTransition t;
        t.profileId = profile;
        t.activityId = a.value().activityId;
        t.expectedRevision = a.value().revision;
        t.targetState = ActivityState::Paused;
        t.reason = "checkpoint";
        QVERIFY(f.activities.transitionActivity(t));
        for (int i = 0; i < 10; ++i) {
            intent.goal = "new" + QString::number(i);
            auto recent = f.activities.startActivity(intent);
            QVERIFY(recent);
            t.activityId = recent.value().activityId;
            t.expectedRevision = recent.value().revision;
            t.targetState = ActivityState::Cancelled;
            t.reason = "done";
            QVERIFY(f.activities.transitionActivity(t));
        }
        auto r = request();
        r.runtimeDatabasePath = f.path;
        r.memoryDatabasePath = f.dir.filePath("empty.db");
        r.cues.clear();
        auto prepared = ActivitySelector::prepare(r);
        QVERIFY(prepared);
        QVERIFY(prepared.value().activities.size() <= 8);
        QCOMPARE(prepared.value().activities.first().activityId,
                 a.value().activityId);
    }
    void autonomyMemoryAccess_defaultDeniedExplicitAndMalformedFailClosed() {
        auto defaults = AutonomyConfig::fromJson({});
        QVERIFY(!defaults.memoryAccessOwner);
        QVERIFY(defaults.memoryAccessConversationIds.isEmpty());
        auto enabled = AutonomyConfig::fromJson(
            {{"memoryAccess",
              QJsonObject{{"owner", true},
                          {"conversationIds", QJsonArray{"room", "room"}}}}});
        QVERIFY(enabled.memoryAccessOwner);
        QCOMPARE(enabled.memoryAccessConversationIds, QStringList{"room"});
        QJsonArray many;
        for (int i = 0; i < 33; ++i)
            many.append(QString::number(i));
        auto excessive = AutonomyConfig::fromJson(
            {{"memoryAccess",
              QJsonObject{{"owner", true}, {"conversationIds", many}}}});
        QVERIFY(!excessive.memoryAccessOwner);
        QVERIFY(excessive.memoryAccessConversationIds.isEmpty());
        auto bad = AutonomyConfig::fromJson(
            {{"memoryAccess",
              QJsonObject{{"owner", true},
                          {"conversationIds", QJsonArray{7}}}}});
        QVERIFY(!bad.memoryAccessOwner);
        QVERIFY(bad.memoryAccessConversationIds.isEmpty());
    }
    void
    applySelection_newInputCatalogAndActualUsageInvalidateOldChoice_data() {
        QTest::addColumn<int>("change");
        QTest::newRow("input") << 0;
        QTest::newRow("catalog") << 1;
        QTest::newRow("actual-usage") << 2;
    }
    void applySelection_newInputCatalogAndActualUsageInvalidateOldChoice() {
        QFETCH(int, change);
        SelectionFixture f;
        auto r = f.offered();
        auto p = f.start(r);
        if (change == 0) {
            auto s = f.signal();
            s.readiness.hostRevision = 2;
            s.inputWatermark = {{"local_owner", "new-input"}};
            QVERIFY(f.opportunities.publishReadiness(s));
        }
        if (change == 1)
            ++f.catalog;
        if (change == 2) {
            ModelBudgetRequest budget;
            budget.profileId = profile;
            budget.attemptId = "actual";
            budget.ticketId = r.ticket.ticketId;
            budget.purpose = "selection";
            budget.maxOutputTokens = 100;
            budget.clock = OpportunityStore::systemClock();
            QVERIFY(f.opportunities.reserveModelAttempt(budget));
            ModelBudgetSettlement settlement;
            settlement.profileId = profile;
            settlement.attemptId = budget.attemptId;
            settlement.clock = budget.clock;
            settlement.usage.totalTokens = f.config.maxTokensPerHour + 1;
            QVERIFY(f.opportunities.settleModelAttempt(settlement));
        }
        auto rejected = f.activities.applySelection({r, p});
        QVERIFY(!rejected);
        QVERIFY(f.activities.list().value().isEmpty());
        QCOMPARE(rejected.error().code, change == 0 ? QString("STALE_CONTEXT")
                                        : change == 1
                                            ? QString("STALE_REVISION")
                                            : QString("BUDGET_EXHAUSTED"));
    }
    void
    applySelection_checkpointResume_preservesBudgetAndContext_userAndRestartDenied() {
        for (const auto &reason : QStringList{"checkpoint", "user", "restart",
                                              "budget", "dependency"}) {
            SelectionFixture f;
            ActivityIntent intent;
            intent.context.profileId = profile;
            intent.context.allowInternalMemory = true;
            intent.goal = "Read pages";
            intent.origin = "self";
            auto started = f.activities.startActivity(intent);
            QVERIFY(started);
            ActivityTransition t;
            t.profileId = profile;
            t.activityId = started.value().activityId;
            t.expectedRevision = started.value().revision;
            t.targetState = ActivityState::Paused;
            t.reason = reason;
            t.executionPhase = "checkpoint";
            t.checkpoint = {{"page", 12}};
            t.activeMs = 137;
            auto paused = f.activities.transitionActivity(t);
            QVERIFY(paused);
            auto r = f.offered();
            r.activities = {paused.value()};
            auto j = proposal();
            j["decision"] = "resume";
            j["activityId"] = paused.value().activityId;
            j["capabilityRefs"] = QJsonArray{};
            j["evidenceRefs"] = QJsonArray{};
            auto p = ActivitySelector::parse(j, r);
            if (reason == "user" || reason == "restart") {
                QVERIFY(!p);
                continue;
            }
            QVERIFY(p);
            auto resumed = f.activities.applySelection({r, p.value()});
            QVERIFY(resumed);
            QCOMPARE(resumed.value().checkpoint["page"].toInt(), 12);
            QCOMPARE(resumed.value().budget.activeMs, qint64(137));
            QCOMPARE(resumed.value().context.cacheKey(),
                     paused.value().context.cacheKey());
        }
    }
    void
    selectActivity_restProviderFailureAndRepairShareActualAttemptBudget_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("rest") << 0;
        QTest::newRow("repair") << 1;
        QTest::newRow("failure") << 2;
        QTest::newRow("fallback-repair-cap") << 3;
    }
    void selectActivity_restProviderFailureAndRepairShareActualAttemptBudget() {
        QFETCH(int, mode);
        QTemporaryDir dir;
        ActivityRuntime runtime(profile, dir.filePath("runtime.db"));
        QSignalSpy ready(&runtime, &ActivityRuntime::ready);
        QTRY_COMPARE(ready.size(), 1);
        SelectionClient client;
        client.mode = mode;
        ModelRoleRegistry registry(selectionRoles());
        ModelRouter router(&registry, &client);
        ActivitySelector selector(&router);
        AutonomyConfig config;
        config.enabled = true;
        runtime.configureOpportunitiesAsync(config);
        std::optional<Result<SelectionProposal, DomainError>> result;
        runtime.setOpportunityConsumer([&](OpportunityTicket ticket) {
            auto r = request();
            r.ticket = ticket;
            r.references.clear();
            r.cues.clear();
            r.capabilities = {};
            selector.selectActivity(
                r, runtime.modelBudgetHooks({}, ticket.ticketId),
                [&](auto p) { result = std::move(p); });
        });
        OpportunitySignal signal;
        signal.clock = OpportunityStore::systemClock();
        signal.readiness.running = true;
        signal.readiness.autonomyEnabled = true;
        signal.readiness.hostRevision = 1;
        runtime.requestOpportunityAsync(signal);
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 5000);
        QCOMPARE(client.calls, mode == 0 ? 1 : 2);
        if (mode <= 1) {
            QVERIFY(*result);
            QCOMPARE(result->value().decision, QString("wait"));
        } else
            QVERIFY(!*result);
        OpportunityStore store(profile, dir.filePath("runtime.db"));
        QVERIFY(!store.snapshot().value().contains("lastCheckedAt"));
    }
    void selectionMaterials_newCommittedMessageWithoutOldMemory() {
        SelectionFixture f;
        ConversationStore conversations(profile, f.path);
        QVERIFY(conversations.initialize());
        HostConversationBinding binding;
        binding.channel = "sample";
        binding.connectionKey = "controlled";
        binding.remoteConversationId = "room";
        QVERIFY(conversations.registerBinding(binding));
        IncomingMessage input;
        input.channel = "sample";
        input.connectionKey = "controlled";
        input.remoteConversationId = "room";
        input.remoteMessageId = "m1";
        input.speakerIdentity = {"sample", "ns", "friend"};
        input.content = "Exam tomorrow, checking in";
        input.occurredAt = QDateTime::currentDateTimeUtc();
        auto accepted = conversations.acceptMessage(input);
        QVERIFY(accepted);
        QVERIFY(conversations.recoverOutbox());
        auto r = request();
        r.runtimeDatabasePath = f.path;
        r.memoryDatabasePath = f.dir.filePath("no-memory.db");
        r.ticket.contextId = accepted.value().conversationId;
        r.ticket.eventIds = {accepted.value().originId};
        r.cues.clear();
        auto material = ActivitySelector::prepare(r);
        QVERIFY(material);
        QVERIFY(material.value().references.contains(
            "event:" + accepted.value().originId));
        QCOMPARE(material.value().cues.first().text, input.content);
        QCOMPARE(material.value().context.originKind, OriginKind::External);
    }
    void selectionMaterials_realSourcesAudienceAndChangedMeaning() {
        QTemporaryDir dir;
        auto runtime = dir.filePath("runtime.db"),
             memory = dir.filePath("memory.db");
        ActivityStore activities(profile, runtime);
        QVERIFY(activities.initialize());
        auto unit = SqliteRuntimeUnitOfWork::begin(runtime);
        QVERIFY(unit);
        {
            QSqlQuery q(QSqlDatabase::database(unit.value()->connectionName()));
            q.prepare("INSERT INTO agent_persons(profile_id,person_id,kind) "
                      "VALUES(?,'friend','person')");
            q.addBindValue(profile);
            QVERIFY(q.exec());
            q.prepare("INSERT INTO "
                      "event_log(event_id,schema_version,profile_id,type,"
                      "source,privacy,payload_json,occurred_at,created_at) "
                      "VALUES('origin',1,?,'MessageObserved','test','normal','{"
                      "}','2026-10-17','2026-10-17')");
            q.addBindValue(profile);
            QVERIFY(q.exec());
        }
        QVERIFY(unit.value()->commit());
        unit.value().reset();
        MemoryStore memories;
        memories.setProfileId(profile);
        memories.setDatabasePath(memory);
        QVERIFY(memories.loadDatabaseOnly());
        MemoryEntry m;
        m.id = "exam";
        m.type = MemoryType::Semantic;
        m.summary = "Friend has an exam";
        m.attribution.profileId = profile;
        m.attribution.resolved = true;
        m.attribution.audience.kind = MemoryAudienceKind::Internal;
        m.attribution.people = {{"friend", MemoryPersonRole::Subject}};
        m.attribution.origins = {{"origin",
                                  {},
                                  "friend",
                                  QDateTime::currentDateTimeUtc(),
                                  "origin",
                                  true}};
        auto saved = memories.addEntry(m);
        QVERIFY(!saved.id.isEmpty());
        auto r = request();
        r.runtimeDatabasePath = runtime;
        r.memoryDatabasePath = memory;
        r.cues.clear();
        r.context.authorizedConversationIds = {"missing-conversation"};
        auto prepared = ActivitySelector::prepare(r);
        QVERIFY(prepared);
        QVERIFY(prepared.value().context.authorizedConversationIds.isEmpty());
        QVERIFY(prepared.value().references.contains("memory:" + saved.id));
        QVERIFY(prepared.value().references.contains("person:friend"));
        auto j = proposal();
        j["evidenceRefs"] =
            QJsonArray{"memory:" + saved.id, "event:origin", "person:friend"};
        QVERIFY(ActivitySelector::parse(j, prepared.value()));
        auto external = r;
        external.context.originKind = OriginKind::External;
        external.context.allowInternalMemory = false;
        external.cues = {
            {"thought:secret", "thought", "private thought", {"event:origin"}}};
        auto restricted = ActivitySelector::prepare(external);
        QVERIFY(restricted);
        QVERIFY(restricted.value().cues.isEmpty());
        QVERIFY(!ActivitySelector::parse(j, restricted.value()));
        saved.summary = "The exam was cancelled";
        QVERIFY(memories.updateEntryById(saved));
        QVERIFY(!prepared.value().validateSources());
    }
    void
    applySelection_ticketConsumptionAndForegroundAcquisition_atomicRollback() {
        QTemporaryDir dir;
        const auto path = dir.filePath("runtime.db");
        ActivityStore store(profile, path);
        QVERIFY(store.initialize());
        store.setCapabilityValidator([](const auto &, const auto &,
                                        bool autonomous, quint64) {
            return autonomous ? Result<void, DomainError>::success()
                              : Result<void, DomainError>::failure(domainError(
                                    "PERMISSION_DENIED", "autonomy missing"));
        });
        OpportunityStore opportunities(profile, path);
        AutonomyConfig config;
        config.enabled = true;
        config.minDecisionIntervalMs = 1;
        auto clock = OpportunityStore::systemClock();
        QVERIFY(opportunities.configure(config, clock));
        OpportunitySignal signal;
        signal.clock = clock;
        signal.readiness.running = true;
        signal.readiness.autonomyEnabled = true;
        signal.readiness.hostRevision = 1;
        auto ticket = opportunities.considerOpportunity(signal);
        QVERIFY(ticket);
        auto r = request();
        r.ticket = ticket.value();
        r.runtimeDatabasePath = path;
        r.references.clear();
        r.cues.clear();
        auto j = proposal();
        j["evidenceRefs"] = QJsonArray{};
        auto p = ActivitySelector::parse(j, r);
        QVERIFY(p);
        auto tx = SqliteRuntimeUnitOfWork::begin(path);
        QVERIFY(tx);
        {
            QSqlQuery q(QSqlDatabase::database(tx.value()->connectionName()));
            QVERIFY(q.exec(
                "CREATE TRIGGER fail_adopt BEFORE INSERT ON agent_activities "
                "BEGIN SELECT RAISE(ABORT,'injected'); END"));
        }
        QVERIFY(tx.value()->commit());
        tx.value().reset();
        auto failed = store.applySelection({r, p.value()});
        QVERIFY(!failed);
        QCOMPARE(opportunities.snapshot()
                     .value()["selectionReservation"]
                     .toObject()["ticketId"]
                     .toString(),
                 r.ticket.ticketId);
        QVERIFY(store.list().value().isEmpty());
        tx = SqliteRuntimeUnitOfWork::begin(path);
        QVERIFY(tx);
        {
            QSqlQuery q(QSqlDatabase::database(tx.value()->connectionName()));
            QVERIFY(q.exec("DROP TRIGGER fail_adopt"));
        }
        QVERIFY(tx.value()->commit());
        tx.value().reset();
        auto accepted = store.applySelection({r, p.value()});
        QVERIFY2(accepted,
                 qPrintable(accepted ? QString() : accepted.error().message));
        QCOMPARE(accepted.value().state, ActivityState::Running);
        QVERIFY(opportunities.snapshot()
                    .value()["selectionReservation"]
                    .toObject()
                    .isEmpty());
        QVERIFY(!store.applySelection({r, p.value()}));
    }
    void selectActivity_emptyMaterial_hasValidRestSchema() {
        auto r = request();
        r.references.clear();
        r.cues.clear();
        r.capabilities = {};
        auto j = proposal();
        j["decision"] = "wait";
        j["evidenceRefs"] = QJsonArray{};
        j["capabilityRefs"] = QJsonArray{};
        QVERIFY(ActivitySelector::parse(j, r));
        QVERIFY(!QJsonDocument(ActivitySelector::schema(r))
                     .toJson()
                     .contains("\"enum\": [\n        ]"));
    }
    void selectActivity_relevantFriendMemory_proposesEvidenceBasedCheck() {
        auto r = ActivitySelector::parse(proposal(), request());
        QVERIFY(r);
        QCOMPARE(r.value().intent.origin, QString("self"));
        QCOMPARE(r.value().intent.evidenceRefs.size(), 3);
        QCOMPARE(r.value().intent.context.originKind, OriginKind::Internal);
    }
    void
    selectActivity_unknownReferenceOrWrongAudience_rejectsWithoutDispatch() {
        auto p = proposal();
        p["evidenceRefs"] = QJsonArray{"person:invented"};
        QVERIFY(!ActivitySelector::parse(p, request()));
        p = proposal();
        p["context"] = QJsonObject{{"allowOwnerMemory", true}};
        QVERIFY(!ActivitySelector::parse(p, request()));
    }
    void selectActivity_noSuitableActivity_acceptsRest() {
        auto p = proposal();
        p["decision"] = "wait";
        p["goal"] = "";
        p["capabilityRefs"] = QJsonArray{};
        QVERIFY(ActivitySelector::parse(p, request()));
    }
};
QTEST_GUILESS_MAIN(ActivitySelectorTests)
#include "test_activity_selector.moc"
