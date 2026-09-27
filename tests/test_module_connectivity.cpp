#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>

#include "ai/ai_brain.h"
#include "ai/tools/companion_tools.h"
#include "ai/tools/schedule_tools.h"
#include "ai/event/event_ledger.h"
#include "ai/identity/sqlite_identity_repository.h"
#include "ai/runtime/agent_bootstrap.h"
#include "ai/runtime/agent_runtime_services.h"
#include "ai/runtime/runtime_ui_bridge.h"
#include "ai/reflection/diary_fragment_service.h"
#include "ai/reflection/private_key_provider.h"
#include "ai/reflection/private_psyche_crypto.h"
#include "ai/reflection/sqlite_private_psyche_repository.h"

namespace {
const QString profileId = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QString otherProfileId = QStringLiteral("22222222-2222-4222-8222-222222222222");

class FakeClient : public ModelCompletionClient {
public:
    bool deferred = false;
    QString content = QStringLiteral("收到，我会记得你的偏好。");
    LlmCompletionHandler pending;
    void completeOnce(const ModelRouteConfig&, const QList<ChatMessage>&, const QJsonArray&,
                      LlmCompletionHandler callback, const QString&) override {
        if (deferred) { pending = std::move(callback); return; }
        LlmResponse response;
        response.content = content;
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
    for (auto role : {ModelRole::Dialogue, ModelRole::Daydream, ModelRole::Diary}) {
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
    BubbleTool() : AITool("show_chat_bubble", "test reminder", ToolCategory::Action) {}
    QJsonObject parameterSchema() const override { return {{"type", "object"}}; }
    ToolResult execute(const QJsonObject&) override {
        return succeeds ? ToolResult::ok() : ToolResult::fail("display failed");
    }
};

struct Runtime {
    QTemporaryDir directory;
    FakeClient client;
    AIBrain brain{&client, roles()};
    CallbackRuntimeUiBridge bridge{RuntimeUiCallbacks{}, nullptr, nullptr};
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
    void proactiveModeControlsBrainAndExpires();
    void reminderStateFollowsSnoozeCancelAndDelivery();
    void reminderFailureRetriesWithoutSuccessAndPartialDeliveryCompletes();
    void reminderMemoryOutboxSurvivesRestart();
    void profileSchedulersHaveExclusiveOwnership();
    void corruptScheduleIsNotOverwritten();
    void chatFeedsRelationshipAndGrowthSurvivesRestart();
    void growthWaitsForRealEvidenceWindowAndIgnoresTemporaryRequests();
    void manualDaydreamBypassesInitialIdleAndCancelsOnChat();
    void runtimeStopsFragmentsBeforeTheirRepository();
    void fragmentSynchronousCompletionAndDeletionDuringRequest();
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
        .promptSlots.value("persona_traits").contains(QStringLiteral("更乐于交流")));
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
        event.payload = {{"text", text}, {"triggerTag", "user_request"}};
        QVERIFY(runtime.services.eventLedger()->append(event).isOk());
    }
    QVERIFY(runtime.services.processIdentityEvents(now.addSecs(1)).isOk());
    auto snapshot = runtime.services.captureSnapshot("initial");
    QCOMPARE(snapshot.relationshipVersion.value_or(0), 3);
    QVERIFY(!snapshot.personalityVersion.has_value());
    QVERIFY(runtime.services.processIdentityEvents(now.addDays(15)).isOk());
    snapshot = runtime.services.captureSnapshot("grown");
    QCOMPARE(snapshot.personalityVersion.value_or(0), 1);
    QVERIFY(snapshot.selfModelVersion.has_value());
    QVERIFY(runtime.services.projectPersona(snapshot, {"TestPet", now})
        .promptSlots.value("persona_traits").contains(QStringLiteral("长期表达")));
    const auto self = snapshot.selfModelVersion;
    QVERIFY(runtime.services.processIdentityEvents(now.addDays(15)).isOk());
    snapshot = runtime.services.captureSnapshot("replayed");
    QCOMPARE(snapshot.personalityVersion.value_or(0), 1);
    QCOMPARE(snapshot.selfModelVersion, self);
}

void TestModuleConnectivity::manualDaydreamBypassesInitialIdleAndCancelsOnChat() {
    Runtime runtime;
    QVERIFY(runtime.ready);
    runtime.client.deferred = true;
    MemoryEntry entry;
    entry.type = MemoryType::ShortTerm;
    entry.source = "user_explicit";
    entry.key = "test";
    entry.content = entry.summary = QStringLiteral("主人最近在学习绘画");
    runtime.brain.memoryStore()->addEntry(entry);
    runtime.brain.start();
    const auto started = runtime.brain.requestManualDaydream();
    QVERIFY(started.isOk());
    QVERIFY(started.value());
    QVERIFY(runtime.brain.canContinueDaydream());
    QSignalSpy cancelled(&runtime.brain, &AIBrain::daydreamCancelled);
    runtime.brain.triggerThink(QStringLiteral("你好"), "user_request");
    QCOMPARE(cancelled.count(), 1);
    QVERIFY(!runtime.brain.m_daydreamRunning);
    runtime.brain.stop();
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

QTEST_GUILESS_MAIN(TestModuleConnectivity)
#include "test_module_connectivity.moc"
