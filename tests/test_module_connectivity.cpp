#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>

#include "ai/ai_brain.h"
#include "ai/identity/persona_projector.h"
#include "ai/identity/person_directory.h"
#include "ai/tools/companion_tools.h"
#include "ai/tools/schedule_tools.h"
#include "ai/event/event_ledger.h"
#include "ai/identity/sqlite_identity_repository.h"
#include "ai/runtime/agent_bootstrap.h"
#include "ai/runtime/agent_runtime_services.h"
#include "ai/runtime/runtime_ui_bridge.h"
#include "ai/reflection/diary_fragment_service.h"
#include "ai/reflection/cancellation_token.h"
#include "ai/reflection/private_key_provider.h"
#include "ai/reflection/private_psyche_crypto.h"
#include "ai/reflection/sqlite_private_psyche_repository.h"
#include "ai/task/task_orchestrator.h"
#include "ai/task/task_experience_adapter.h"

namespace {
const QString profileId = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QString otherProfileId = QStringLiteral("22222222-2222-4222-8222-222222222222");

class FakeClient : public ModelCompletionClient {
public:
    bool deferred = false;
    QString content = QStringLiteral("收到，我会记得你的偏好。");
    QStringList systemPrompts;
    QList<QJsonObject> jsonInputs;
    std::function<QString(const QList<ChatMessage>&)> responder;
    LlmCompletionHandler pending;
    void completeOnce(const ModelRouteConfig&, const QList<ChatMessage>& messages, const QJsonArray&,
                      LlmCompletionHandler callback, const QString&) override {
        systemPrompts.append(messages.isEmpty() ? QString() : messages.first().content);
        if (messages.size() > 1) jsonInputs.append(QJsonDocument::fromJson(messages.last().content.toUtf8()).object());
        if (deferred) { pending = std::move(callback); return; }
        LlmResponse response;
        response.content = responder ? responder(messages) : content;
        callback(true, response, {});
    }
};

QList<ModelRoleConfig> roles() {
    ModelRouteConfig route;
    route.routeId = QStringLiteral("fake");
    route.enabled = route.llm.enabled = true;
    route.llm.provider = QStringLiteral("fake");
    route.llm.baseUrl = QStringLiteral("https://test.invalid");
    route.llm.apiKey = QStringLiteral("test");
    route.llm.model = QStringLiteral("fake");
    route.llm.timeoutMs = 1000;
    QList<ModelRoleConfig> result;
    for (auto role : {ModelRole::Dialogue, ModelRole::Daydream, ModelRole::Diary,
                      ModelRole::AgencyDecision, ModelRole::TaskExecution}) {
        ModelRoleConfig config;
        config.role = role;
        config.routes = {route};
        result.append(config);
    }
    return result;
}

class BubbleTool : public AITool {
public:
    bool succeeds = true;
    int executions = 0;
    BubbleTool() : AITool("show_chat_bubble", "test reminder", ToolCategory::Action) { setHostMetadata(HostToolMetadata::localAction()); }
    QJsonObject parameterSchema() const override { return {{"type", "object"}}; }
    ToolResult execute(const QJsonObject&) override {
        ++executions;
        return succeeds ? ToolResult::ok() : ToolResult::fail("display failed");
    }
};

struct Runtime {
    QTemporaryDir directory;
    FakeClient client;
    AIBrain brain{&client, roles()};
    CallbackRuntimeUiBridge bridge{RuntimeUiCallbacks{}};
    AgentRuntimeServices services;
    RuntimeStartRequest request;
    bool ready = false;
    Runtime() {
        request.profile = {"TestPet", "model.gltf", profileId};
        request.profileMigration.profileId = profileId;
        request.profileMigration.registeredProfileIds = {profileId};
        request.profileMigration.appDataRoot = directory.path();
        request.profileMigration.legacyDatabasePath = directory.filePath("legacy.db");
        request.profileMigration.legacyJsonPath = directory.filePath("legacy.json");
        request.configHash = QString(64, 'a');
        request.identityBaselineHash = QString(64, 'b');
        request.aiBrain = &brain;
        request.uiBridge = &bridge;
        ready = AgentBootstrap::start(services, request).isOk();
        brain.setEnabled(true);
    }
};

QJsonObject reminder(const QString& type = "once_at") {
    return {{"type", type}, {"title", "喝水"}, {"delay_minutes", 1},
            {"interval_minutes", 2}, {"respect_quiet_hours", 0}};
}

MemoryEntry shadow(const MemoryStore& memory, const QString& id) {
    for (const auto& entry : memory.all())
        if (entry.payload.value("linked_task_id").toString() == id) return entry;
    return {};
}

class Keys : public PrivateKeyProvider {
    Result<PrivateKeyMaterial, DomainError> loadOrCreate(const QString& id) override {
        return Result<PrivateKeyMaterial, DomainError>::success({id, 1, QByteArray(32, 'k')});
    }
};
class Crypto : public PrivatePsycheCrypto {
    Result<EncryptedPrivatePayload, DomainError> encrypt(const QByteArray& data,
        const PrivateRecordAad&, const PrivateKeyMaterial&) const override {
        EncryptedPrivatePayload result;
        result.keyVersion = 1;
        result.nonce = QByteArray(24, 'n');
        result.ciphertext = data;
        return Result<EncryptedPrivatePayload, DomainError>::success(result);
    }
    Result<QByteArray, DomainError> decrypt(const EncryptedPrivatePayload& data,
        const PrivateRecordAad&, const PrivateKeyMaterial&) const override {
        return Result<QByteArray, DomainError>::success(data.ciphertext);
    }
};
}

class TestModuleConnectivity : public QObject {
    Q_OBJECT
private slots:
  void autonomyEnabledRoutesLegacyTriggersWithoutConsumerOrModel();
  void animationPolicyUsesConfiguredStateIdsAndLegacyAlias();
  void proactiveModeControlsBrainAndExpires();
  void reminderStateFollowsSnoozeCancelAndDelivery();
  void reminderFailureRetriesWithoutSuccessAndPartialDeliveryCompletes();
  void reminderMemoryOutboxSurvivesRestart();
  void profileSchedulersHaveExclusiveOwnership();
  void corruptScheduleIsNotOverwritten();
  void structurallyInvalidSchedulesArePreserved_data();
  void structurallyInvalidSchedulesArePreserved();
  void chatFeedsRelationshipAndGrowthSurvivesRestart();
  void growthWaitsForRealEvidenceWindowAndIgnoresTemporaryRequests();
  void relationshipUpdatesRequireRelevantEvidenceAndRespectSubjectScope();
  void manualDaydreamAndChatCompleteConcurrently();
  void startupMaintenanceDoesNotWaitForChatIdle();
  void hippocampusBacklogDoesNotDropNewImpressions();
  void runtimeStopsFragmentsBeforeTheirRepository();
  void fragmentSynchronousCompletionAndDeletionDuringRequest();
  void ordinaryChatBypassesAgencyAndComplexTaskVerifies();
  void taskStopRejectsLateAgencyAndReleasesChat();
  void queuedTaskModelDoesNotCrossRuntimeReplacement();
  void taskCannotDispatchAfterReentrantPause();
  void shutdownAndToolRegistrationPreserveRecoveredTask();
  void disabledAiCancelsAndDoesNotRestartTaskReflection();
};

void TestModuleConnectivity::proactiveModeControlsBrainAndExpires() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    QVERIFY(runtime.brain.canStartProactiveChat());
    SetProactiveModeTool tool({}, runtime.brain.proactiveStatePath());
    QVERIFY(tool.execute({{"mode", "focus"}, {"quiet_minutes", 2}}).success);
    QVERIFY(!runtime.brain.canStartProactiveChat()); // Automatic screen chat uses this same gate.
    QCOMPARE(CompanionProactiveState::mode(runtime.brain.proactiveStatePath(),
        QDateTime::currentDateTimeUtc().addSecs(121)), QStringLiteral("normal"));
    QVERIFY(CompanionProactiveState::setMode("focus", nullptr, 1,
        runtime.brain.proactiveStatePath(), QDateTime::currentDateTimeUtc().addSecs(-61)));
    QVERIFY(runtime.brain.canStartProactiveChat());
    const int normal = runtime.brain.proactiveChatTiming(300000).intervalMs;
    QVERIFY(tool.execute({{"mode", "lively"}}).success);
    QVERIFY(runtime.brain.proactiveChatTiming(300000).intervalMs < normal);
    QVERIFY(tool.execute({{"mode", "quiet"}}).success);
    QVERIFY(!runtime.brain.canStartProactiveChat());
    Runtime other;
    QVERIFY(other.brain.canStartProactiveChat());
}

void TestModuleConnectivity::reminderStateFollowsSnoozeCancelAndDelivery() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    QVERIFY(CompanionProactiveState::setMode(
        "quiet", nullptr, 2, runtime.brain.proactiveStatePath()));
    AgentScheduler scheduler;
    scheduler.setStoragePath(runtime.directory.filePath("tasks.json"));
    QVERIFY(scheduler.load());
    connectSchedulerMemory(scheduler, *runtime.brain.memoryStore());
    ToolRegistry registry;
    registry.registerTool(std::make_unique<BubbleTool>());
    scheduler.setToolRegistry(&registry);
    scheduler.start();
    ScheduleCreateTool create(&scheduler, runtime.brain.memoryStore());
    const auto created = create.execute(reminder());
    QVERIFY(created.success);
    const QString id = created.data.value("task").toObject().value("id").toString();
    QVERIFY(!id.isEmpty());
    QCOMPARE(shadow(*runtime.brain.memoryStore(), id).status, MemoryStatus::Active);
    QVERIFY(DaydreamConsolidator(*runtime.brain.memoryStore()).createSnapshot(8).isEmpty());
    ScheduleSnoozeTool snooze(&scheduler, runtime.brain.memoryStore());
    QVERIFY(snooze.execute({{"id", id}, {"minutes", 5}}).success);
    QCOMPARE(shadow(*runtime.brain.memoryStore(), id).payload.value("next_trigger_at").toString(),
        scheduler.tasks().first().nextTriggerAt.toUTC().toString(Qt::ISODate));
    scheduler.m_tasks.first().nextTriggerAt = QDateTime::currentDateTime().addSecs(-1);
    QSignalSpy success(&scheduler, &AgentScheduler::taskTriggered);
    scheduler.checkDueTasks();
    QCOMPARE(success.count(), 1);
    QVERIFY(scheduler.tasks().isEmpty());
    QCOMPARE(shadow(*runtime.brain.memoryStore(), id).status, MemoryStatus::Archived);
    QVERIFY(shadow(*runtime.brain.memoryStore(), id).payload.value("next_trigger_at").toString().isEmpty());
    const auto next = scheduler.createTask(reminder("interval"));
    QVERIFY(!next.id.isEmpty());
    scheduler.m_lastProactiveAt = {};
    scheduler.m_tasks.first().nextTriggerAt = QDateTime::currentDateTime().addSecs(-1);
    scheduler.checkDueTasks();
    QCOMPARE(shadow(*runtime.brain.memoryStore(), next.id).payload.value("next_trigger_at").toString(),
        scheduler.tasks().first().nextTriggerAt.toUTC().toString(Qt::ISODate));
    ScheduleCancelTool cancel(&scheduler, runtime.brain.memoryStore());
    QVERIFY(cancel.execute({{"id", next.id}}).success);
    QCOMPARE(shadow(*runtime.brain.memoryStore(), next.id).status, MemoryStatus::Cancelled);
}

void TestModuleConnectivity::reminderFailureRetriesWithoutSuccessAndPartialDeliveryCompletes() {
    QTemporaryDir directory;
    AgentScheduler scheduler;
    scheduler.setStoragePath(directory.filePath("tasks.json"));
    ToolRegistry registry;
    auto bubble = std::make_unique<BubbleTool>();
    auto* controlled = bubble.get();
    controlled->succeeds = false;
    registry.registerTool(std::move(bubble));
    scheduler.setToolRegistry(&registry);
    scheduler.start();
    auto params = reminder();
    params["animation_state"] = "Missing";
    QVERIFY(!scheduler.createTask(params).id.isEmpty());
    scheduler.m_tasks.first().nextTriggerAt = QDateTime::currentDateTime().addSecs(-1);
    QSignalSpy failed(&scheduler, &AgentScheduler::taskFailed);
    QSignalSpy succeeded(&scheduler, &AgentScheduler::taskTriggered);
    QSignalSpy partial(&scheduler, &AgentScheduler::taskPartiallySucceeded);
    scheduler.checkDueTasks();
    QCOMPARE(failed.count(), 1);
    QCOMPARE(succeeded.count(), 0);
    QCOMPARE(scheduler.tasks().size(), 1);
    QVERIFY(!scheduler.tasks().first().lastTriggeredAt.isValid());
    QVERIFY(scheduler.tasks().first().nextTriggerAt > QDateTime::currentDateTime());
    controlled->succeeds = true;
    scheduler.m_tasks.first().nextTriggerAt = QDateTime::currentDateTime().addSecs(-1);
    scheduler.checkDueTasks();
    QCOMPARE(failed.count(), 1);
    QCOMPARE(succeeded.count(), 1);
    QCOMPARE(partial.count(), 1);
    QVERIFY(scheduler.tasks().isEmpty());
}

void TestModuleConnectivity::reminderMemoryOutboxSurvivesRestart() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    const auto path = runtime.directory.filePath("tasks.json");
    QString id;
    {
        AgentScheduler scheduler;
        scheduler.setStoragePath(path);
        scheduler.setStateSink([](const QJsonObject&) { return false; });
        ToolRegistry registry;
        registry.registerTool(std::make_unique<BubbleTool>());
        scheduler.setToolRegistry(&registry);
        scheduler.start();
        id = scheduler.createTask(reminder()).id;
        scheduler.m_tasks.first().nextTriggerAt = QDateTime::currentDateTime().addSecs(-1);
        scheduler.checkDueTasks();
        QVERIFY(scheduler.tasks().isEmpty());
        QVERIFY(!scheduler.synchronizeState());
    }
    AgentScheduler restarted;
    restarted.setStoragePath(path);
    QVERIFY(restarted.load());
    connectSchedulerMemory(restarted, *runtime.brain.memoryStore());
    QVERIFY(restarted.synchronizeState());
    QCOMPARE(shadow(*runtime.brain.memoryStore(), id).status, MemoryStatus::Archived);
    const int count = runtime.brain.memoryStore()->all().size();
    QVERIFY(restarted.synchronizeState());
    QCOMPARE(runtime.brain.memoryStore()->all().size(), count);
}

void TestModuleConnectivity::profileSchedulersHaveExclusiveOwnership() {
    QTemporaryDir directory;
    const QStringList profiles{profileId, otherProfileId};
    AgentScheduler first, duplicate, other;
    QVERIFY(first.configureProfileStorage(directory.path(), profileId, profiles));
    QVERIFY(!first.createTask(reminder()).id.isEmpty());
    QVERIFY(!duplicate.configureProfileStorage(directory.path(), profileId, profiles));
    QVERIFY(duplicate.createTask(reminder()).id.isEmpty());
    QVERIFY(other.configureProfileStorage(directory.path(), otherProfileId, profiles));
    QVERIFY(other.tasks().isEmpty());
    QVERIFY(first.storagePath() != other.storagePath());
}

void TestModuleConnectivity::corruptScheduleIsNotOverwritten() {
    QTemporaryDir directory;
    const QString path = directory.filePath("tasks.json");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("broken"); file.close();
    AgentScheduler scheduler;
    scheduler.setStoragePath(path);
    QVERIFY(!scheduler.load());
    QVERIFY(scheduler.createTask(reminder()).id.isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("broken"));
}

void TestModuleConnectivity::structurallyInvalidSchedulesArePreserved_data() {
    QTest::addColumn<QByteArray>("payload");
    ScheduledTask task;
    task.id = "valid-task";
    task.title = task.message = "reminder";
    task.onceAt = task.nextTriggerAt = QDateTime::currentDateTime().addSecs(3600);
    const auto valid = task.toJson();
    const auto add = [](const char* name, const QJsonObject& root) {
        QTest::newRow(name) << QJsonDocument(root).toJson();
    };
    add("missing-tasks", {});
    add("future-version", {{"version", 99}, {"tasks", QJsonArray{valid}}});
    add("tasks-wrong-type", {{"tasks", QJsonObject{{"saved", valid}}}});
    add("invalid-row-alongside-valid", {{"tasks", QJsonArray{valid, QJsonObject{}}}});
    add("duplicate-id", {{"tasks", QJsonArray{valid, valid}}});
    add("pending-wrong-type", {{"tasks", QJsonArray{valid}}, {"pending_states", "damaged"}});
    auto pending = valid;
    pending["status"] = "unknown";
    add("invalid-pending-status", {{"tasks", QJsonArray{valid}}, {"pending_states", QJsonArray{pending}}});
    pending["status"] = "active";
    add("duplicate-pending-id", {{"tasks", QJsonArray{valid}}, {"pending_states", QJsonArray{pending, pending}}});
    auto unsupportedAction = valid;
    auto actions = unsupportedAction.value("actions").toArray();
    actions.append(QJsonObject{{"tool", "unsupported_action"}, {"arguments", QJsonObject{}}});
    unsupportedAction["actions"] = actions;
    add("unsupported-action", {{"tasks", QJsonArray{unsupportedAction}}});
    auto badTimestamp = valid;
    badTimestamp["next_trigger_at"] = "not-a-time";
    add("invalid-due-date", {{"tasks", QJsonArray{badTimestamp}}});
    auto badPolicy = valid;
    badPolicy["policy"] = QJsonObject{{"respect_quiet_hours", "false"}};
    add("invalid-policy-type", {{"tasks", QJsonArray{badPolicy}}});
    auto badSource = valid;
    badSource["source"] = 3;
    add("invalid-source", {{"tasks", QJsonArray{badSource}}});
    auto badPriority = valid;
    badPriority["priority"] = "urgent";
    add("invalid-priority", {{"tasks", QJsonArray{badPriority}}});
    add("completed-wrong-type", {{"tasks", QJsonArray{valid}}, {"completed_tasks", QJsonObject{}}});
    add("invalid-completed-row", {{"tasks", QJsonArray{}}, {"completed_tasks", QJsonArray{valid}}});
}

void TestModuleConnectivity::structurallyInvalidSchedulesArePreserved() {
    QFETCH(QByteArray, payload);
    QTemporaryDir directory;
    const QString path = directory.filePath("tasks.json");
    AgentScheduler scheduler;
    scheduler.setStoragePath(path);
    const auto original = scheduler.createTask(reminder());
    QVERIFY(!original.id.isEmpty());
    scheduler.start();
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(payload), payload.size());
    file.close();
    QVERIFY(!scheduler.load());
    QVERIFY(!scheduler.isRunning());
    QCOMPARE(scheduler.tasks().size(), 1);
    QCOMPARE(scheduler.tasks().first().id, original.id);
    scheduler.setStateSink([](const QJsonObject&) { return true; });
    QVERIFY(!scheduler.save());
    QVERIFY(scheduler.createTask(reminder()).id.isEmpty());
    QVERIFY(!ScheduleListTool(&scheduler).execute({}).success);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), payload);
}

void TestModuleConnectivity::chatFeedsRelationshipAndGrowthSurvivesRestart() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    runtime.brain.triggerThink(QStringLiteral("多陪我聊天"), "user_request");
    QTRY_VERIFY_WITH_TIMEOUT(runtime.services.captureSnapshot("after-chat").relationshipVersion.has_value(), 3000);
    const auto relationshipVersion = runtime.services.captureSnapshot("before-restart").relationshipVersion;
    QVERIFY(!runtime.services.captureSnapshot("before-restart").personalityVersion.has_value());
    runtime.services.stop();
    AIBrain restartedBrain(&runtime.client, roles());
    auto request = runtime.request;
    request.aiBrain = &restartedBrain;
    AgentRuntimeServices restarted;
    QVERIFY(AgentBootstrap::start(restarted, request).isOk());
    QVERIFY(restarted.processIdentityEvents().isOk());
    QCOMPARE(restarted.captureSnapshot("after-restart").relationshipVersion, relationshipVersion);
    QVERIFY(restarted.projectPersona(restarted.captureSnapshot("projection"), {"TestPet", {}})
        .promptSlots.value("persona_traits").contains(QStringLiteral("逐渐了解彼此")));
}

void TestModuleConnectivity::growthWaitsForRealEvidenceWindowAndIgnoresTemporaryRequests() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto& text : {QStringLiteral("多陪我聊天"), QStringLiteral("喜欢和你聊天"),
                             QStringLiteral("谢谢你"), QStringLiteral("我喜欢喝茶"), QStringLiteral("现在不要主动聊天")}) {
        EventDraft event;
        event.profileId = profileId;
        event.type = "UserMessageReceived";
        event.source = "test";
        event.occurredAt = now;
        event.payload = {{"text", text}, {"triggerTag", "user_request"}, {"origin_kind", "local_owner"}};
        QVERIFY(runtime.services.eventLedger()->append(event).isOk());
    }
    QVERIFY(runtime.services.processIdentityEvents(now.addSecs(1)).isOk());
    auto snapshot = runtime.services.captureSnapshot("initial");
    QCOMPARE(snapshot.relationshipVersion.value_or(0), 7);
    QVERIFY(!snapshot.personalityVersion.has_value());
    QVERIFY(runtime.services.processIdentityEvents(now.addDays(15)).isOk());
    snapshot = runtime.services.captureSnapshot("grown");
    // Reading the same one-day experiences later cannot manufacture a 14-day
    // evidence span, and runtime event processing never publishes personality.
    QVERIFY(!snapshot.personalityVersion.has_value());
    QVERIFY(!snapshot.selfModelVersion.has_value());
    SqliteIdentityRepository repository;
    QVERIFY(repository.open(runtime.services.chatPreparationRuntimeMetadata().runtimeDatabasePath).isOk());
    const auto pending = repository.pendingEvidence(profileId, now.addDays(-1), now.addDays(1));
    QVERIFY(pending.isOk());
    QCOMPARE(pending.value().size(), 3);
    for (const auto& item : pending.value()) {
        QCOMPARE(item.trait, QStringLiteral("sociability"));
        QVERIFY(!item.originalSourceId.isEmpty());
        QCOMPARE(item.category, TraitEvidenceCategory::Fact);
    }
    const auto relationshipVersion = snapshot.relationshipVersion;
    QVERIFY(runtime.services.processIdentityEvents(now.addDays(15)).isOk());
    snapshot = runtime.services.captureSnapshot("replayed");
    QVERIFY(!snapshot.personalityVersion.has_value());
    QCOMPARE(snapshot.relationshipVersion, relationshipVersion);
}

void TestModuleConnectivity::relationshipUpdatesRequireRelevantEvidenceAndRespectSubjectScope() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    SqliteIdentityRepository repository;
    QVERIFY(repository.open(runtime.services.chatPreparationRuntimeMetadata().runtimeDatabasePath).isOk());
    const auto appendUser = [&](const QString& text) {
        EventDraft event;
        event.profileId = profileId;
        event.type = QStringLiteral("UserMessageReceived");
        event.source = QStringLiteral("test");
        event.payload = {{QStringLiteral("text"), text}, {QStringLiteral("triggerTag"), QStringLiteral("user_request")},
            {QStringLiteral("origin_kind"), QStringLiteral("local_owner")}};
        return runtime.services.eventLedger()->append(event).isOk();
    };
    QVERIFY(appendUser(QStringLiteral("我今天在练习水彩")));
    QVERIFY(runtime.services.processIdentityEvents().isOk());
    auto relationship = repository.currentRelationship(profileId, QStringLiteral("owner"));
    QVERIFY(relationship.isOk() && relationship.value().has_value());
    QCOMPARE(relationship.value()->tendencies.value(QStringLiteral("familiarity")), 0.102);
    QCOMPARE(relationship.value()->tendencies.value(QStringLiteral("trust")), 0.45);
    QCOMPARE(relationship.value()->tendencies.value(QStringLiteral("affinity")), 0.50);
    QVERIFY(appendUser(QStringLiteral("你说得很准确")));
    QVERIFY(appendUser(QStringLiteral("你可以拒绝")));
    QVERIFY(runtime.services.processIdentityEvents().isOk());
    relationship = repository.currentRelationship(profileId, QStringLiteral("owner"));
    QVERIFY(relationship.isOk() && relationship.value().has_value());
    QCOMPARE(relationship.value()->tendencies.value(QStringLiteral("trust")), 0.462);
    QCOMPARE(relationship.value()->tendencies.value(QStringLiteral("boundaryRespect")), 0.612);
    const auto ownerVersion = relationship.value()->version;
    QVERIFY(runtime.services.personDirectory());
    const auto guestResolution = runtime.services.personDirectory()->resolvePerson({
        profileId, ExternalIdentity{QStringLiteral("test-chat"), QStringLiteral("global"),
            QStringLiteral("guest-account"), QStringLiteral("guest")}, false});
    QVERIFY(guestResolution.isOk() && guestResolution.value().personId.has_value());
    const QString guestId = *guestResolution.value().personId;
    QVERIFY(guestId != QLatin1String("owner"));
    const auto appendTask = [&](const QString& status) {
        EventDraft event;
        event.profileId = profileId;
        event.type = QStringLiteral("TaskExperienceRecorded");
        event.source = QStringLiteral("test");
        event.payload = {{QStringLiteral("task_id"), QStringLiteral("task-") + status},
            {QStringLiteral("goal"), QStringLiteral("整理旅行方案")}, {QStringLiteral("status"), status},
            {QStringLiteral("feedback_status"), QStringLiteral("unknown")}, {QStringLiteral("subject_id"), guestId},
            {QStringLiteral("origin_kind"), QStringLiteral("external")}};
        return runtime.services.eventLedger()->append(event).isOk();
    };
    QVERIFY(appendTask(QStringLiteral("Failed")));
    QVERIFY(runtime.services.processIdentityEvents().isOk());
    auto guest = repository.currentRelationship(profileId, guestId);
    QVERIFY(guest.isOk() && guest.value().has_value());
    QCOMPARE(guest.value()->tendencies.value(QStringLiteral("familiarity")), 0.105);
    QCOMPARE(guest.value()->tendencies.value(QStringLiteral("trust")), 0.45);
    const auto guestVersion = guest.value()->version;
    QVERIFY(appendTask(QStringLiteral("Declined")));
    QVERIFY(runtime.services.processIdentityEvents(QDateTime::currentDateTimeUtc().addDays(30)).isOk());
    guest = repository.currentRelationship(profileId, guestId);
    QVERIFY(guest.isOk() && guest.value().has_value());
    QCOMPARE(guest.value()->version, guestVersion);
    relationship = repository.currentRelationship(profileId, QStringLiteral("owner"));
    QVERIFY(relationship.isOk() && relationship.value().has_value());
    QCOMPARE(relationship.value()->version, ownerVersion);
}

void TestModuleConnectivity::manualDaydreamAndChatCompleteConcurrently() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    runtime.client.deferred = true;
    MemoryEntry entry;
    entry.type = MemoryType::ShortTerm;
    entry.source = "user_explicit";
    entry.key = "test";
    entry.content = entry.summary = QStringLiteral("主人最近在学习绘画");
    const auto source = runtime.brain.memoryStore()->addEntry(entry);
    runtime.brain.start();
    const auto started = runtime.brain.requestManualDaydream();
    QVERIFY(started.isOk());
    QVERIFY(started.value());
    QTRY_VERIFY(bool(runtime.client.pending));
    auto consolidation = std::move(runtime.client.pending);
    QSignalSpy cancelled(&runtime.brain, &AIBrain::daydreamCancelled);
    QSignalSpy finished(&runtime.brain, &AIBrain::daydreamFinished);
    runtime.brain.triggerThink(QStringLiteral("我最近在学习水彩，聊一聊配色原理吧"), "user_request");
    QTRY_VERIFY(bool(runtime.client.pending));
    auto dialogue = std::move(runtime.client.pending);
    QCOMPARE(cancelled.count(), 0);
    QVERIFY(runtime.brain.m_daydreamRunning);
    QVERIFY(runtime.brain.isBusy());
    LlmResponse memoryResponse;
    memoryResponse.content = QString::fromUtf8(QJsonDocument(QJsonArray{
        QJsonObject{{"source_id", source.id},
                    {"action", "create"}, {"target_partition", "Semantic"},
                    {"merged_content", "主人最近在学习绘画"}, {"quality_score", 7},
                    {"new_tags", QJsonArray{}}}}).toJson());
    consolidation(true, memoryResponse, {});
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(runtime.brain.isBusy()); // Model dialogue is still in flight.
    LlmResponse response;
    response.content = QStringLiteral("最近天气不错。");
    dialogue(true, response, {});
    QTRY_VERIFY(!runtime.brain.isBusy());
    QTRY_VERIFY(!runtime.brain.m_daydreamRunning);
    QCOMPARE(cancelled.count(), 0);
    QVERIFY(runtime.brain.memoryStore()->refreshDatabaseOnly());
    int consolidated = 0, pending = 0;
    for (const auto& memory : runtime.brain.memoryStore()->all()) {
        if (memory.status == MemoryStatus::Consolidated) ++consolidated;
        if (memory.partition == "hippocampus" && memory.status == MemoryStatus::Active) ++pending;
        QVERIFY(memory.source != "daydream_interruption");
    }
    QCOMPARE(consolidated, 1);
    QVERIFY(pending >= 1); // New conversation memory was not part of the frozen batch.
    runtime.brain.stop();
}

void TestModuleConnectivity::startupMaintenanceDoesNotWaitForChatIdle() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    runtime.client.deferred = true;
    const auto source = runtime.brain.memoryStore()->addEntry(MemoryExtractor().extractDaydreamImpression(
        QStringLiteral("我最近在学习水彩"), QStringLiteral("manual")));
    runtime.brain.start();
    runtime.brain.m_busy = true;
    QTRY_VERIFY_WITH_TIMEOUT(bool(runtime.client.pending), 3000);
    QVERIFY(runtime.brain.m_daydreamRunning);
    runtime.brain.stop();
    LlmResponse late;
    late.content = QString::fromUtf8(QJsonDocument(QJsonArray{
        QJsonObject{{"source_id", source.id}, {"action", "discard"}}}).toJson());
    runtime.client.pending(true, late, {});
    QCOMPARE(runtime.brain.memoryStore()->readForRecall(source.id)->status, MemoryStatus::Active);
}

void TestModuleConnectivity::hippocampusBacklogDoesNotDropNewImpressions() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    for (int i = 0; i < 200; ++i) {
        MemoryEntry old;
        old.type = MemoryType::ShortTerm;
        old.key = QString::number(i);
        old.content = old.summary = QStringLiteral("旧的待整理印象 %1").arg(i);
        QVERIFY(!runtime.brain.memoryStore()->addEntry(old).id.isEmpty());
    }
    runtime.brain.enqueueUserMemoryWrite(QStringLiteral("我最近在学习陶艺"), "user_request",
        "capacity-request", 1, "capacity-session");
    QTRY_COMPARE(runtime.brain.memoryStore()->loadRecentFromDatabase(512, "hippocampus", true).size(), 201);
}

void TestModuleConnectivity::runtimeStopsFragmentsBeforeTheirRepository() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    runtime.services.m_privateRepository = std::make_unique<SqlitePrivatePsycheRepository>();
    runtime.services.m_diaryFragmentService = std::make_unique<DiaryFragmentService>(profileId,
        &runtime.services, nullptr, nullptr, nullptr, nullptr, runtime.services.m_privateRepository.get(), nullptr);
    auto* fragment = runtime.services.diaryFragmentService();
    QPointer<DiaryFragmentService> guard(fragment);
    bool dependencyAliveOnDestruction = false;
    connect(fragment, &QObject::destroyed, this, [&]() {
        dependencyAliveOnDestruction = bool(runtime.services.m_privateRepository);
    });
    fragment->start();
    runtime.services.stop();
    QVERIFY(guard.isNull());
    QVERIFY(dependencyAliveOnDestruction);
    runtime.services.stop();
}

void TestModuleConnectivity::fragmentSynchronousCompletionAndDeletionDuringRequest() {
    QTemporaryDir directory;
    FakeClient client;
    client.content = QStringLiteral("{\"body\":\"今天一起聊了天。\"}");
    ModelRoleRegistry registry(roles());
    ModelRouter router(&registry, &client);
    Keys keys;
    Crypto crypto;
    SqlitePrivatePsycheRepository repository;
    QVERIFY(repository.open(directory.filePath("private.db")).isOk());
    auto service = std::make_unique<DiaryFragmentService>(profileId, nullptr, &router, nullptr,
        &keys, &crypto, &repository, nullptr);
    auto result = service->composeFragment(QDate::currentDate(), 0, 1, 2, {});
    QVERIFY2(result.isOk(), result.isOk() ? "" : qPrintable(result.error().message));
    client.deferred = true;
    QTimer::singleShot(0, this, [&]() { service.reset(); });
    result = service->composeFragment(QDate::currentDate(), 1, 3, 4, {});
    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("CANCELLED"));
    QVERIFY(client.pending);
    LlmResponse late;
    late.content = client.content;
    client.pending(true, late, {}); // Late completion owns no dangling stack references.
}

void TestModuleConnectivity::ordinaryChatBypassesAgencyAndComplexTaskVerifies() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    runtime.brain.triggerThink(QStringLiteral("我最近在学习绘画，聊聊配色原理吧"), "user_request");
    QTRY_VERIFY(!runtime.brain.isBusy());
    QVERIFY(runtime.brain.taskSnapshot().isEmpty());
    QVERIFY(std::none_of(runtime.client.systemPrompts.begin(), runtime.client.systemPrompts.end(),
        [](const QString& prompt) { return prompt.contains("subjective agency"); }));
    runtime.client.systemPrompts.clear();
    MemoryEntry preference;
    preference.type = MemoryType::Preference;
    preference.summary = preference.content = "PUBLIC_WALKING_PREFERENCE";
    preference.privacyLevel = PrivacyLevel::Personal;
    QVERIFY(!runtime.brain.memoryStore()->addEntry(preference).id.isEmpty());
    preference.id.clear(); preference.key = "sensitive";
    preference.summary = preference.content = "PRIVATE_ONLY_MARKER";
    preference.privacyLevel = PrivacyLevel::Sensitive;
    QVERIFY(!runtime.brain.memoryStore()->addEntry(preference).id.isEmpty());
    runtime.client.responder = [](const QList<ChatMessage>& messages) {
        const auto prompt = messages.first().content;
        QJsonObject response;
        if (prompt.contains("subjective agency"))
            response = {{"decision", "accept"}, {"reason", "精力充足"}, {"message", "我来整理。"}};
        else if (prompt.contains("Independently verify"))
            response = {{"complete", true}, {"supported", true}, {"contradictions", QJsonArray{}},
                {"unresolved", QJsonArray{}}, {"evidence", QJsonArray{"user_input"}}, {"result", "一、先看资料；二、列出重点。"}};
        else response = {{"required_source_tools",QJsonArray{}},{"plan", "整理给定文字"}, {"completion_criteria", "覆盖原文两项内容"},
            {"candidate_answer", "一、先看资料；二、列出重点。"}};
        return QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
    };
    runtime.brain.triggerThink(QStringLiteral("/task 请为以下文字列提纲：先看资料，然后列出重点。"), "user_request");
    QTRY_COMPARE(runtime.brain.taskSnapshot().value("status").toString(), QString("Completed"));
    QTRY_VERIFY(!runtime.brain.isBusy());
    QCOMPARE(std::count_if(runtime.client.systemPrompts.begin(), runtime.client.systemPrompts.end(),
        [](const QString& prompt) { return prompt.contains("subjective agency") || prompt.contains("Plan one next step")
            || prompt.contains("Independently verify"); }), 3);
    QVERIFY(runtime.client.systemPrompts.at(0).contains("subjective agency"));
    QVERIFY(runtime.client.systemPrompts.at(2).contains("Independently verify"));
    QVERIFY(runtime.brain.m_taskModelRequests.isEmpty());
    const QString memories = runtime.client.jsonInputs.last().value("memory_material").toString();
    QVERIFY(memories.contains("PUBLIC_WALKING_PREFERENCE"));
    QVERIFY(!memories.contains("PRIVATE_ONLY_MARKER"));
    QVERIFY(memories.size() <= 4000);
}

void TestModuleConnectivity::queuedTaskModelDoesNotCrossRuntimeReplacement() {
    Runtime runtime;QVERIFY(runtime.ready);runtime.client.deferred=true;
    runtime.brain.triggerThink("/task summarize supplied material","user_request");
    runtime.brain.shutdownTaskRuntime();runtime.brain.initializeTaskRuntime();
    QTest::qWait(100);QVERIFY(runtime.client.systemPrompts.isEmpty());QVERIFY(!runtime.client.pending);
}

void TestModuleConnectivity::taskStopRejectsLateAgencyAndReleasesChat() {
    Runtime runtime; QVERIFY(runtime.ready);
    runtime.client.deferred = true;
    runtime.brain.triggerThink(QStringLiteral("/task 请梳理我提供的文字"), "user_request");
    QVERIFY(runtime.brain.isTaskActive());
    QTRY_VERIFY(bool(runtime.client.pending));
    auto late = std::move(runtime.client.pending);
    runtime.brain.stopCurrentResponse();
    QTRY_COMPARE(runtime.brain.taskSnapshot().value("status").toString(), QString("Cancelled"));
    QVERIFY(!runtime.brain.isBusy());
    LlmResponse response;
    response.content = R"({"decision":"accept","reason":"ready","message":"continue"})";
    late(true, response, {});
    QCoreApplication::processEvents();
    QTRY_COMPARE(runtime.brain.taskSnapshot().value("status").toString(), QString("Cancelled"));
    QCOMPARE(runtime.client.systemPrompts.size(), 1);
}

void TestModuleConnectivity::taskCannotDispatchAfterReentrantPause() {
    Runtime runtime; QVERIFY(runtime.ready);
    ToolRegistry registry;
    auto bubble = std::make_unique<BubbleTool>();
    auto* controlled = bubble.get();
    registry.registerTool(std::move(bubble));
    runtime.brain.setToolRegistry(&registry);
    runtime.client.responder = [](const QList<ChatMessage>& messages) {
        const QJsonObject response = messages.first().content.contains("subjective agency")
            ? QJsonObject{{"decision", "accept"}, {"reason", "ready"}, {"message", "开始"}}
            : QJsonObject{{"required_source_tools",QJsonArray{}},{"plan", "展示内容"}, {"completion_criteria", "内容已经显示"},
                {"tool", QJsonObject{{"name", "show_chat_bubble"}, {"arguments", QJsonObject{{"text", "test"}}}}}};
        return QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
    };
    QSignalSpy toolCalls(&runtime.brain, &AIBrain::toolExecuted);
    connect(&runtime.brain, &AIBrain::taskProgressChanged, &runtime.brain, [&](const QJsonObject& task) {
        if (task.value("status").toString() == "Executing") runtime.brain.pauseTask();
    });
    runtime.brain.triggerThink(QStringLiteral("/task 分两步向我展示内容"), "user_request");
    QTRY_COMPARE(runtime.brain.taskSnapshot().value("status").toString(), QString("Paused"));
    QCoreApplication::processEvents();
    QVERIFY(!runtime.brain.isBusy());
    QCOMPARE(toolCalls.size(), 0);
    QCOMPARE(controlled->executions, 0);
    runtime.brain.stopTask();
    runtime.brain.setToolRegistry(nullptr);
}

void TestModuleConnectivity::shutdownAndToolRegistrationPreserveRecoveredTask() {
    Runtime runtime; QVERIFY(runtime.ready);
    runtime.client.deferred = true;
    runtime.brain.triggerThink(QStringLiteral("/task 请梳理我提供的文字"), "user_request");
    QTRY_VERIFY(!runtime.brain.taskSnapshot().isEmpty());
    const auto started = runtime.brain.taskSnapshot();
    QVERIFY(runtime.brain.isTaskActive());
    runtime.brain.stop(); // Window close uses shutdown semantics, not user cancellation.
    QTRY_COMPARE(runtime.brain.taskSnapshot().value("status").toString(), QString("Paused"));
    QVERIFY(!runtime.brain.isBusy());
    const auto paused = runtime.brain.taskSnapshot();
    runtime.brain.shutdownTaskRuntime();
    runtime.brain.initializeTaskRuntime();
    QTRY_COMPARE(runtime.brain.taskSnapshot().value("status").toString(), QString("Paused"));
    QCOMPARE(runtime.brain.taskSnapshot().value("id"), started.value("id"));
    QCOMPARE(runtime.brain.taskSnapshot().value("personality_snapshot"), started.value("personality_snapshot"));
    QCOMPARE(runtime.brain.taskSnapshot().value("active_ms"), paused.value("active_ms"));
    ToolRegistry registry;
    registry.registerTool(std::make_unique<BubbleTool>());
    runtime.brain.setToolRegistry(&registry); // Production attaches tools after bootstrap.
    QTRY_COMPARE(runtime.brain.taskSnapshot().value("status").toString(), QString("Paused"));
    runtime.brain.setToolRegistry(nullptr);
    QTRY_COMPARE(runtime.brain.taskSnapshot().value("status").toString(), QString("Paused"));
}

void TestModuleConnectivity::disabledAiCancelsAndDoesNotRestartTaskReflection() {
    Runtime runtime; QVERIFY(runtime.ready);
    QVERIFY(runtime.services.m_personalityCancellation);
    const auto evolutionToken = runtime.services.m_personalityCancellation->token();
    QVERIFY(runtime.brain.m_taskExperienceTimer.isActive());
    auto* adapter = runtime.brain.m_taskExperienceAdapter.get();
    QVERIFY(adapter);
    QSignalSpy drained(adapter, &TaskExperienceAdapter::drained);
    adapter->drain();
    QVERIFY(adapter->busy());
    runtime.brain.setEnabled(false);
    QVERIFY(evolutionToken.isCancelled());
    QVERIFY(!runtime.brain.m_taskExperienceTimer.isActive());
    QVERIFY(!adapter->busy());
    QCoreApplication::processEvents(); // Includes the previously queued startup drain.
    QVERIFY(!adapter->busy());
    QCOMPARE(drained.size(), 0);
    QVERIFY(runtime.client.systemPrompts.isEmpty());
    runtime.brain.setEnabled(true);
    runtime.brain.start();
    QVERIFY(runtime.brain.m_taskExperienceTimer.isActive());
    QTRY_VERIFY(drained.size() >= 1);
    runtime.brain.stop();
    QVERIFY(!runtime.brain.m_taskExperienceTimer.isActive());
}


void TestModuleConnectivity::animationPolicyUsesConfiguredStateIdsAndLegacyAlias() {
    AIBrain brain;
    LlmToolCall alias;alias.name="request_idle_transition";alias.arguments={{"target_action","cloud_perch"}};
    QString denial;
    QVERIFY2(brain.isToolCallAllowed("user_request",alias,denial),qPrintable(denial));
    LlmToolCall arbitrary;arbitrary.name="play_animation";arbitrary.arguments={{"state","TouchOfCloud"}};
    QVERIFY2(brain.isToolCallAllowed("user_request",arbitrary,denial),qPrintable(denial));
}
void TestModuleConnectivity::autonomyEnabledRoutesLegacyTriggersWithoutConsumerOrModel() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    AutonomyConfig config;
    config.enabled = true;
    runtime.brain.setAutonomyConfig(config);
    runtime.brain.start();
    runtime.brain.triggerThink("legacy tick", "proactive_chat");
    runtime.brain.triggerThink("idle tick", "idle_action");
    QTest::qWait(100);
    QVERIFY(runtime.client.systemPrompts.isEmpty());
    QVERIFY(!runtime.brain.canStartProactiveChat());
    QVERIFY(!runtime.brain.m_idleTriggerTimer.isActive());
    QVERIFY(!runtime.brain.m_chatTriggerTimer.isActive());
    runtime.brain.setAutonomyPaused(true);
    runtime.brain.requestAutonomousOpportunity("message");
    QTest::qWait(30);
    QVERIFY(runtime.client.systemPrompts.isEmpty());
    runtime.brain.stop();
}

QTEST_GUILESS_MAIN(TestModuleConnectivity)
#include "test_module_connectivity.moc"
