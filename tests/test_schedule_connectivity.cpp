#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "ai/ai_brain.h"
#include "ai/scheduler/agent_scheduler.h"
#include "ai/tools/companion_tools.h"
#include "ai/tools/schedule_tools.h"

namespace {
class ModelClient final : public ModelCompletionClient {
public:
    int calls = 0;
    void completeOnce(const ModelRouteConfig&, const QList<ChatMessage>&,
                      const QJsonArray&, LlmCompletionHandler complete,
                      const QString&) override {
        ++calls;
        LlmResponse response;
        response.content = QStringLiteral("请确认提醒时间。");
        complete(true, response, {});
    }
};

QList<ModelRoleConfig> modelRoles() {
    ModelRouteConfig route;
    route.routeId = QStringLiteral("test");
    route.enabled = route.llm.enabled = true;
    route.llm.provider = QStringLiteral("fake");
    route.llm.baseUrl = QStringLiteral("https://test.invalid");
    route.llm.apiKey = QStringLiteral("test");
    route.llm.model = QStringLiteral("fake");
    route.llm.timeoutMs = 1000;
    ModelRoleConfig config;
    config.role = ModelRole::Dialogue;
    config.routes = {route};
    return {config};
}

struct Environment {
    QTemporaryDir directory;
    ModelClient client;
    AIBrain brain{&client, modelRoles()};
    AgentScheduler scheduler;
    ToolRegistry registry;
    bool ready = false;

    Environment() {
        scheduler.setStoragePath(directory.filePath(QStringLiteral("tasks.json")));
        ready = brain.initializeStorage({directory.filePath(QStringLiteral("memory.db")),
                                         directory.filePath(QStringLiteral("memory.json"))}).isOk()
            && scheduler.load();
        if (!ready) return;
        connectSchedulerMemory(scheduler, *brain.memoryStore());
        registry.registerTool(std::make_unique<ScheduleCreateTool>(&scheduler, brain.memoryStore()));
        registry.registerTool(std::make_unique<ScheduleListTool>(&scheduler));
        registry.registerTool(std::make_unique<ScheduleSnoozeTool>(&scheduler, brain.memoryStore()));
        registry.registerTool(std::make_unique<ScheduleCancelTool>(&scheduler, brain.memoryStore()));
        scheduler.setToolRegistry(&registry);
        brain.setToolRegistry(&registry);
        brain.setAgentScheduler(&scheduler);
        brain.setEnabled(true);
    }

    ~Environment() {
        scheduler.stop();
        brain.setAgentScheduler(nullptr);
        brain.setToolRegistry(nullptr);
        brain.stop();
    }
};

MemoryEntry shadow(const MemoryStore& store, const QString& id) {
    for (const auto& entry : store.all()) {
        if (entry.type == MemoryType::TaskShadow
            && entry.payload.value("linked_task_id").toString() == id) return entry;
    }
    return {};
}
}

class TestScheduleConnectivity : public QObject {
    Q_OBJECT
private slots:
    void userMessageCreatesAndListsTheActualReminder();
    void ambiguousTimeReachesModelWithoutCreatingDefaultTask();
    void unavailableStorageIsShownAsFailure();
    void busyBubbleRetriesAndSimultaneousRemindersDoNotOverwrite();

private:
    static void makeDue(AgentScheduler& scheduler) {
        for (auto& task : scheduler.m_tasks) {
            task.nextTriggerAt = QDateTime::currentDateTime().addSecs(-1);
        }
    }
};

void TestScheduleConnectivity::userMessageCreatesAndListsTheActualReminder() {
    Environment env;
    QVERIFY(env.ready);
    QString response;
    connect(&env.brain, &AIBrain::assistantResponseDelta, this,
        [&response](const QString&, const QString& delta) { response += delta; });
    QSignalSpy finished(&env.brain, &AIBrain::assistantResponseFinished);

    env.brain.triggerThink(QStringLiteral("5分钟后提醒我给电脑充电"), "user_request");
    QCOMPARE(finished.count(), 1);
    QCOMPARE(env.client.calls, 0);
    QCOMPARE(env.scheduler.tasks().size(), 1);
    const auto task = env.scheduler.tasks().first();
    QVERIFY(task.onceAt > QDateTime::currentDateTime().addSecs(295));
    QVERIFY(task.onceAt <= QDateTime::currentDateTime().addSecs(300));
    QVERIFY(response.contains(task.id));
    QVERIFY(response.contains(QStringLiteral("给电脑充电")));
    QVERIFY(response.contains(task.nextTriggerAt.toLocalTime().toString("yyyy-MM-dd HH:mm:ss")));
    QCOMPARE(shadow(*env.brain.memoryStore(), task.id).status, MemoryStatus::Active);

    response.clear();
    env.brain.triggerThink(QStringLiteral("查看提醒"), "user_request");
    QCOMPARE(finished.count(), 2);
    QVERIFY(response.contains(task.id));
    QVERIFY(response.contains(QStringLiteral("给电脑充电")));
    QVERIFY(response.contains(task.nextTriggerAt.toLocalTime().toString("yyyy-MM-dd HH:mm:ss")));
    QCOMPARE(env.client.calls, 0);

    ScheduleSnoozeTool snooze(&env.scheduler, env.brain.memoryStore());
    QVERIFY(snooze.execute({{"id", task.id}, {"minutes", 15}}).success);
    response.clear();
    env.brain.triggerThink(QStringLiteral("查看提醒"), "user_request");
    QVERIFY(response.contains(env.scheduler.tasks().first().nextTriggerAt
        .toLocalTime().toString("yyyy-MM-dd HH:mm:ss")));
    ScheduleCancelTool cancel(&env.scheduler, env.brain.memoryStore());
    QVERIFY(cancel.execute({{"id", task.id}}).success);
    response.clear();
    env.brain.triggerThink(QStringLiteral("查看提醒"), "user_request");
    QVERIFY(response.contains(QStringLiteral("当前没有待提醒任务")));
    QVERIFY(!response.contains(task.id));
}

void TestScheduleConnectivity::ambiguousTimeReachesModelWithoutCreatingDefaultTask() {
    Environment env;
    QVERIFY(env.ready);
    QSignalSpy finished(&env.brain, &AIBrain::assistantResponseFinished);
    env.brain.triggerThink(QStringLiteral("五分钟后提醒我给电脑充电"), "user_request");
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
    QCOMPARE(env.client.calls, 1);
    QVERIFY(env.scheduler.tasks().isEmpty());
}

void TestScheduleConnectivity::unavailableStorageIsShownAsFailure() {
    Environment env;
    QVERIFY(env.ready);
    QVERIFY(!env.scheduler.configureProfileStorage(env.directory.path(), "invalid", {}));
    QString response;
    connect(&env.brain, &AIBrain::assistantResponseDelta, this,
        [&response](const QString&, const QString& delta) { response += delta; });
    QSignalSpy finished(&env.brain, &AIBrain::assistantResponseFinished);
    env.brain.triggerThink(QStringLiteral("查看提醒"), "user_request");
    QCOMPARE(finished.count(), 1);
    QCOMPARE(qvariant_cast<ChatMessageStatus>(finished.first().at(1)), ChatMessageStatus::Failed);
    QVERIFY(response.contains(QStringLiteral("不可用")));
    QVERIFY(!response.contains(QStringLiteral("当前没有待提醒任务")));
}

void TestScheduleConnectivity::busyBubbleRetriesAndSimultaneousRemindersDoNotOverwrite() {
    Environment env;
    QVERIFY(env.ready);
    bool streaming = true;
    bool occupied = false;
    QStringList displayed;
    env.registry.registerTool(std::make_unique<ShowChatBubbleTool>(
        [&](const QString& text, int) {
            if (streaming || occupied) return false;
            displayed.append(text);
            occupied = true;
            return true;
        }));
    const auto first = env.scheduler.createTask({{"type", "once_at"}, {"title", "喝水"},
        {"delay_minutes", 1}, {"respect_quiet_hours", 0}});
    const auto second = env.scheduler.createTask({{"type", "once_at"}, {"title", "休息"},
        {"delay_minutes", 1}, {"respect_quiet_hours", 0}});
    QVERIFY(!first.id.isEmpty());
    QVERIFY(!second.id.isEmpty());
    QSignalSpy delivered(&env.scheduler, &AgentScheduler::taskTriggered);
    QSignalSpy failed(&env.scheduler, &AgentScheduler::taskFailed);
    env.scheduler.start();
    makeDue(env.scheduler);
    env.scheduler.checkDueTasks();
    QCOMPARE(delivered.count(), 0);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(displayed.size(), 0);
    QCOMPARE(env.scheduler.tasks().size(), 2);
    QVERIFY(env.scheduler.completedTasks().isEmpty());
    for (const auto& task : env.scheduler.tasks()) {
        QVERIFY(!task.lastTriggeredAt.isValid());
        QCOMPARE(shadow(*env.brain.memoryStore(), task.id).status, MemoryStatus::Active);
    }

    streaming = false;
    makeDue(env.scheduler);
    env.scheduler.checkDueTasks();
    QCOMPARE(delivered.count(), 1);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(displayed.size(), 1);
    QCOMPARE(env.scheduler.tasks().size(), 1);
    QCOMPARE(env.scheduler.completedTasks().size(), 1);
    const auto pendingId = env.scheduler.tasks().first().id;
    QCOMPARE(shadow(*env.brain.memoryStore(), pendingId).status, MemoryStatus::Active);

    occupied = false;
    makeDue(env.scheduler);
    env.scheduler.checkDueTasks();
    QCOMPARE(delivered.count(), 2);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(displayed.size(), 2);
    QVERIFY(displayed.contains(QStringLiteral("喝水")));
    QVERIFY(displayed.contains(QStringLiteral("休息")));
    QVERIFY(env.scheduler.tasks().isEmpty());
    QCOMPARE(shadow(*env.brain.memoryStore(), first.id).status, MemoryStatus::Archived);
    QCOMPARE(shadow(*env.brain.memoryStore(), second.id).status, MemoryStatus::Archived);

    QString response;
    connect(&env.brain, &AIBrain::assistantResponseDelta, this,
        [&response](const QString&, const QString& delta) { response += delta; });
    env.brain.triggerThink(QStringLiteral("查看提醒"), "user_request");
    QVERIFY(response.contains(QStringLiteral("最近已完成")));
    QVERIFY(response.contains(first.id));
    QVERIFY(response.contains(second.id));
}

QTEST_GUILESS_MAIN(TestScheduleConnectivity)
#include "test_schedule_connectivity.moc"
