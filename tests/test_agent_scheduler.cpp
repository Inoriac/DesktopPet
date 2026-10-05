#include <QtTest>

#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>

#include <limits>

#include "ai/scheduler/agent_scheduler.h"

namespace {

QDateTime clockAt(const char* value = "2026-10-05T12:00:00Z") {
    return QDateTime::fromString(QString::fromLatin1(value), Qt::ISODate);
}

ScheduledTask taskAt(const QString& id, const QDateTime& due,
                     const QString& type = "once_at", const QString& source = "user_request") {
    ScheduledTask task;
    task.id = task.title = task.message = id;
    task.source = source;
    task.triggerType = type;
    task.onceAt = due;
    task.intervalMs = 120000;
    task.respectQuietHours = false;
    task.createdAt = task.updatedAt = due.addSecs(-3600);
    task.nextTriggerAt = due;
    return task;
}

class BubbleTool final : public AITool {
public:
    explicit BubbleTool(QStringList* delivered)
        : AITool("show_chat_bubble", "record reminder delivery", ToolCategory::Action)
        , m_delivered(delivered) {}
    QJsonObject parameterSchema() const override { return {{"type", "object"}}; }
    ToolResult execute(const QJsonObject& parameters) override {
        m_delivered->append(parameters.value("text").toString());
        return ToolResult::ok();
    }
private:
    QStringList* m_delivered;
};

class ControlledTool final : public AITool {
public:
    ControlledTool(const QString& name, QStringList* calls, ToolResult* result)
        : AITool(name, "controlled reminder action", ToolCategory::Action)
        , m_name(name), m_calls(calls), m_result(result) {}
    QJsonObject parameterSchema() const override { return {{"type", "object"}}; }
    ToolResult execute(const QJsonObject&) override {
        m_calls->append(m_name);
        return *m_result;
    }
private:
    QString m_name;
    QStringList* m_calls;
    ToolResult* m_result;
};

struct Fixture {
    QTemporaryDir directory;
    QStringList delivered;
    ToolRegistry registry;
    AgentScheduler scheduler;
    bool ready = false;
    Fixture() {
        registry.registerTool(std::make_unique<BubbleTool>(&delivered));
        scheduler.setStoragePath(directory.filePath("tasks.json"));
        scheduler.setToolRegistry(&registry);
        ready = directory.isValid() && scheduler.load();
        if (ready) scheduler.start();
    }
};

} // namespace

class TestAgentScheduler : public QObject {
    Q_OBJECT
private slots:
    void longMinuteDurationsRemainPositiveAndSurviveSerialization();
    void invalidDurationInputs_data();
    void invalidDurationInputs();
    void invalidSnoozeDoesNotMutateTask();
    void unrepresentableTimestampsCannotOverwriteStorage();
    void hugeSnoozeCannotRemoveCompletedHistory();
    void dailySecondsAndTimestampMillisecondsSurviveSerialization();
    void deferredReminderWaitsForTextBeforePlayingAnimation();
    void failedReminderTextDoesNotPlayAnimation();
    void userRemindersKeepTheirCadenceAndDoNotConsumeProactiveCooldown();
    void proactivePeersDoNotStarveBehindAnIntervalTask_data();
    void proactivePeersDoNotStarveBehindAnIntervalTask();
    void perTaskMinimumGapDoesNotDelayOtherReminders();
    void busyPolicyWaitsForAnAvailableIdleProvider();
    void quietHoursResumeAtEight_data();
    void quietHoursResumeAtEight();
    void completedOnceReminderCanBeSnoozedAfterRestart();
    void completedHistoryRetainsOnlyTheLatestHundred();
    void stateSinkPreservesDisabledAndTerminalStates();
    void storageAvailabilityReportsExclusiveOwnership();
};

void TestAgentScheduler::longMinuteDurationsRemainPositiveAndSurviveSerialization() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    constexpr qint64 thirtyDaysMs = qint64(30) * 24 * 60 * 60 * 1000;
    const auto once = fixture.scheduler.createTask({{"type", "once_at"}, {"title", "month"},
                                                    {"delay_minutes", 43200}});
    QVERIFY(!once.id.isEmpty());
    QCOMPARE(once.createdAt.msecsTo(once.onceAt), thirtyDaysMs);
    const auto interval = fixture.scheduler.createTask({{"type", "interval"}, {"title", "monthly"},
                                                        {"interval_minutes", 43200}});
    QVERIFY(!interval.id.isEmpty());
    QCOMPARE(interval.intervalMs, thirtyDaysMs);
    const auto restored = ScheduledTask::fromJson(interval.toJson());
    QCOMPARE(restored.intervalMs, thirtyDaysMs);
    QCOMPARE(restored.nextTriggerAt, interval.nextTriggerAt);

    const QDateTime before = QDateTime::currentDateTime();
    QVERIFY(fixture.scheduler.snoozeTask(once.id, 43200));
    const QDateTime after = QDateTime::currentDateTime();
    const auto snoozed = fixture.scheduler.tasks().first();
    QVERIFY(snoozed.nextTriggerAt >= before.addMSecs(thirtyDaysMs));
    QVERIFY(snoozed.nextTriggerAt <= after.addMSecs(thirtyDaysMs));
    QCOMPARE(snoozed.onceAt, snoozed.nextTriggerAt);
}

void TestAgentScheduler::invalidDurationInputs_data() {
    QTest::addColumn<QJsonObject>("parameters");
    QTest::newRow("interval-missing") << QJsonObject{{"type", "interval"}, {"title", "invalid"}};
    for (const QString key : {QStringLiteral("interval_minutes"), QStringLiteral("interval_ms")}) {
        for (const double value : {0.0, -1.0, 1.5, 9007199254740992.0}) {
            const QByteArray name = QStringLiteral("%1-%2").arg(key).arg(value, 0, 'g', 17).toUtf8();
            QTest::newRow(name.constData()) << QJsonObject{{"type", "interval"}, {"title", "invalid"},
                                                          {key, value}};
        }
    }
    QTest::newRow("minute-product-outside-json-range")
        << QJsonObject{{"type", "interval"}, {"title", "invalid"}, {"interval_minutes", 150119987580.0}};
    QTest::newRow("interval-too-short")
        << QJsonObject{{"type", "interval"}, {"title", "invalid"}, {"interval_ms", 59999}};
    QTest::newRow("interval-string")
        << QJsonObject{{"type", "interval"}, {"title", "invalid"}, {"interval_minutes", "2"}};
    QTest::newRow("once-negative")
        << QJsonObject{{"type", "once_at"}, {"title", "invalid"}, {"delay_minutes", -1}};
    QTest::newRow("once-outside-iso-date-range")
        << QJsonObject{{"type", "once_at"}, {"title", "invalid"},
                       {"delay_minutes", 9007199254740991LL / 60000}};
    QTest::newRow("interval-outside-iso-date-range")
        << QJsonObject{{"type", "interval"}, {"title", "invalid"},
                       {"interval_ms", 9007199254740991LL}};
    QTest::newRow("gap-fractional")
        << QJsonObject{{"type", "interval"}, {"title", "invalid"}, {"interval_minutes", 2},
                       {"min_gap_ms", 0.5}};
}

void TestAgentScheduler::invalidDurationInputs() {
    QFETCH(QJsonObject, parameters);
    Fixture fixture;
    QVERIFY(fixture.ready);
    QString error;
    QVERIFY(fixture.scheduler.createTask(parameters, &error).id.isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(fixture.scheduler.tasks().isEmpty());
}

void TestAgentScheduler::invalidSnoozeDoesNotMutateTask() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const auto original = fixture.scheduler.createTask({{"type", "once_at"}, {"title", "water"},
                                                        {"delay_minutes", 5}});
    QVERIFY(!original.id.isEmpty());
    QFile file(fixture.scheduler.storagePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto originalBytes = file.readAll();
    file.close();
    for (const qint64 minutes : {qint64(0), qint64(-1), qint64(9007199254740991LL / 60000),
                                std::numeric_limits<qint64>::max()}) {
        QString error;
        QVERIFY(!fixture.scheduler.snoozeTask(original.id, minutes, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(fixture.scheduler.tasks().first().toJson(), original.toJson());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), originalBytes);
        file.close();
    }
    QVERIFY(fixture.scheduler.load());
    QCOMPARE(fixture.scheduler.tasks().first().toJson(), original.toJson());
}

void TestAgentScheduler::unrepresentableTimestampsCannotOverwriteStorage() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const auto original = fixture.scheduler.createTask({{"type", "once_at"}, {"title", "water"},
                                                        {"delay_minutes", 5}});
    QVERIFY(!original.id.isEmpty());
    QFile file(fixture.scheduler.storagePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto originalBytes = file.readAll();
    file.close();
    const QDateTime beyondIso = clockAt().addYears(10000);
    QVERIFY(beyondIso.isValid());
    QVERIFY(beyondIso.toString(Qt::ISODateWithMs).isEmpty());
    const QList<QDateTime ScheduledTask::*> timestamps{
        &ScheduledTask::onceAt, &ScheduledTask::createdAt, &ScheduledTask::updatedAt,
        &ScheduledTask::lastTriggeredAt, &ScheduledTask::nextTriggerAt};
    for (const auto timestamp : timestamps) {
        auto broken = original;
        broken.*timestamp = beyondIso;
        QVERIFY(!broken.isValid());
        fixture.scheduler.m_tasks = {broken};
        QVERIFY(!fixture.scheduler.save());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), originalBytes);
        file.close();
    }
    fixture.scheduler.m_tasks = {original};
    QVERIFY(fixture.scheduler.load());
    QCOMPARE(fixture.scheduler.tasks().first().toJson(), original.toJson());
}

void TestAgentScheduler::hugeSnoozeCannotRemoveCompletedHistory() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const auto now = clockAt();
    fixture.scheduler.m_tasks = {taskAt("completed", now)};
    fixture.scheduler.checkDueTasksAt(now);
    const auto completed = fixture.scheduler.completedTasks().first();
    QFile file(fixture.scheduler.storagePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto originalBytes = file.readAll();
    file.close();
    QString error;
    QVERIFY(!fixture.scheduler.snoozeTask(completed.id, 9007199254740991LL / 60000, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(fixture.scheduler.tasks().isEmpty());
    QCOMPARE(fixture.scheduler.completedTasks().first().toJson(), completed.toJson());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), originalBytes);
    file.close();
    QVERIFY(fixture.scheduler.load());
    QCOMPARE(fixture.scheduler.completedTasks().first().toJson(), completed.toJson());
}

void TestAgentScheduler::dailySecondsAndTimestampMillisecondsSurviveSerialization() {
    auto task = taskAt("daily", clockAt());
    task.triggerType = "daily_at";
    task.dailyAt = QTime(12, 34, 56);
    task.createdAt = task.createdAt.addMSecs(123);
    task.updatedAt = task.createdAt;
    task.refreshNextTrigger(clockAt());
    const auto restored = ScheduledTask::fromJson(task.toJson());
    QCOMPARE(restored.dailyAt, task.dailyAt);
    QCOMPARE(restored.createdAt, task.createdAt);
    auto next = restored;
    next.refreshNextTrigger(clockAt("2026-10-05T12:34:57Z"));
    QCOMPARE(next.nextTriggerAt, clockAt("2026-10-06T12:34:56Z"));
}

void TestAgentScheduler::deferredReminderWaitsForTextBeforePlayingAnimation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QStringList calls;
    ToolResult bubble = ToolResult::fail("UI busy");
    bubble.data["delivery_deferred"] = true;
    ToolResult animation = ToolResult::ok();
    ToolRegistry registry;
    registry.registerTool(std::make_unique<ControlledTool>("show_chat_bubble", &calls, &bubble));
    registry.registerTool(std::make_unique<ControlledTool>("play_animation", &calls, &animation));
    AgentScheduler scheduler;
    scheduler.setStoragePath(directory.filePath("tasks.json"));
    scheduler.setToolRegistry(&registry);
    QVERIFY(scheduler.load());
    scheduler.start();
    const auto now = clockAt();
    auto task = taskAt("busy", now, "once_at", "proactive");
    task.animationState = "Happy";
    scheduler.m_tasks = {task};
    QSignalSpy failed(&scheduler, &AgentScheduler::taskFailed);
    QSignalSpy triggered(&scheduler, &AgentScheduler::taskTriggered);
    QSignalSpy partial(&scheduler, &AgentScheduler::taskPartiallySucceeded);

    scheduler.checkDueTasksAt(now);
    QCOMPARE(calls, QStringList{"show_chat_bubble"});
    QCOMPARE(failed.size(), 0);
    QCOMPARE(triggered.size(), 0);
    QVERIFY(scheduler.completedTasks().isEmpty());
    QCOMPARE(scheduler.tasks().first().nextTriggerAt, now.addSecs(60));
    QCOMPARE(scheduler.m_pendingStates.value(task.id).value("outcome").toString(), QString("deferred"));
    QVERIFY(!scheduler.tasks().first().lastTriggeredAt.isValid());
    QVERIFY(!scheduler.m_lastProactiveAt.isValid());

    bubble = ToolResult::ok();
    animation = ToolResult::fail("optional animation unavailable");
    scheduler.checkDueTasksAt(now.addSecs(60));
    QCOMPARE(calls, QStringList({"show_chat_bubble", "show_chat_bubble", "play_animation"}));
    QCOMPARE(failed.size(), 0);
    QCOMPARE(triggered.size(), 1);
    QCOMPARE(partial.size(), 1);
    QVERIFY(scheduler.tasks().isEmpty());
    QCOMPARE(scheduler.completedTasks().size(), 1);
}

void TestAgentScheduler::failedReminderTextDoesNotPlayAnimation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QStringList calls;
    ToolResult bubble = ToolResult::fail("delivery error");
    ToolResult animation = ToolResult::ok();
    ToolRegistry registry;
    registry.registerTool(std::make_unique<ControlledTool>("show_chat_bubble", &calls, &bubble));
    registry.registerTool(std::make_unique<ControlledTool>("play_animation", &calls, &animation));
    AgentScheduler scheduler;
    scheduler.setStoragePath(directory.filePath("tasks.json"));
    scheduler.setToolRegistry(&registry);
    QVERIFY(scheduler.load());
    scheduler.start();
    const auto now = clockAt();
    auto task = taskAt("failed", now);
    task.animationState = "Happy";
    scheduler.m_tasks = {task};
    QSignalSpy failed(&scheduler, &AgentScheduler::taskFailed);
    scheduler.checkDueTasksAt(now);
    QCOMPARE(calls, QStringList{"show_chat_bubble"});
    QCOMPARE(failed.size(), 1);
    QCOMPARE(scheduler.tasks().first().nextTriggerAt, now.addSecs(60));
    QCOMPARE(scheduler.m_pendingStates.value(task.id).value("outcome").toString(), QString("retry_pending"));
    QVERIFY(scheduler.completedTasks().isEmpty());
}

void TestAgentScheduler::userRemindersKeepTheirCadenceAndDoNotConsumeProactiveCooldown() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const auto now = clockAt();
    fixture.scheduler.m_tasks = {taskAt("user-interval", now, "interval"),
                                 taskAt("user-once", now),
                                 taskAt("proactive", now, "once_at", "proactive")};
    fixture.scheduler.checkDueTasksAt(now);
    QCOMPARE(fixture.delivered.size(), 3);
    QCOMPARE(fixture.scheduler.m_lastProactiveAt, now);

    fixture.scheduler.m_tasks.append(taskAt("next-proactive", now.addSecs(120), "once_at", "proactive"));
    fixture.scheduler.checkDueTasksAt(now.addSecs(120));
    QCOMPARE(fixture.delivered.count("user-interval"), 2);
    QCOMPARE(fixture.delivered.count("next-proactive"), 0);
    QCOMPARE(fixture.scheduler.m_lastProactiveAt, now);
    for (const auto& task : fixture.scheduler.tasks()) {
        if (task.id == "user-interval") QCOMPARE(task.nextTriggerAt, now.addSecs(240));
        if (task.id == "next-proactive") QCOMPARE(task.nextTriggerAt, now.addSecs(600));
    }
}

void TestAgentScheduler::proactivePeersDoNotStarveBehindAnIntervalTask_data() {
    QTest::addColumn<int>("intervalPriority");
    QTest::newRow("same-priority") << 50;
    QTest::newRow("higher-priority-interval") << 80;
}

void TestAgentScheduler::proactivePeersDoNotStarveBehindAnIntervalTask() {
    QFETCH(int, intervalPriority);
    Fixture fixture;
    QVERIFY(fixture.ready);
    const auto now = clockAt();
    fixture.scheduler.m_tasks = {taskAt("a-interval", now, "interval", "proactive"),
                                 taskAt("b-once", now, "once_at", "proactive")};
    fixture.scheduler.m_tasks[0].priority = intervalPriority;
    fixture.scheduler.checkDueTasksAt(now);
    QCOMPARE(fixture.delivered, QStringList{"a-interval"});
    fixture.scheduler.checkDueTasksAt(now.addSecs(120));
    QCOMPARE(fixture.delivered.size(), 1);
    fixture.scheduler.checkDueTasksAt(now.addSecs(600));
    QCOMPARE(fixture.delivered, QStringList({"a-interval", "b-once"}));
    fixture.scheduler.checkDueTasksAt(now.addSecs(1200));
    QCOMPARE(fixture.delivered.count("a-interval"), 2);
}

void TestAgentScheduler::perTaskMinimumGapDoesNotDelayOtherReminders() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const auto now = clockAt();
    auto gapped = taskAt("gapped", now, "interval");
    gapped.minGapMs = 5 * 60000;
    gapped.lastTriggeredAt = now.addSecs(-120);
    fixture.scheduler.m_tasks = {gapped, taskAt("free", now)};
    fixture.scheduler.checkDueTasksAt(now);
    QCOMPARE(fixture.delivered, QStringList{"free"});
    QCOMPARE(fixture.scheduler.tasks().first().nextTriggerAt, now.addSecs(180));
    fixture.scheduler.checkDueTasksAt(now.addSecs(180));
    QCOMPARE(fixture.delivered, QStringList({"free", "gapped"}));
}

void TestAgentScheduler::busyPolicyWaitsForAnAvailableIdleProvider() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const auto now = clockAt();
    auto task = taskAt("wait-for-idle", now);
    task.skipWhenUserBusy = true;
    fixture.scheduler.m_tasks = {task, taskAt("unrestricted", now)};
    fixture.scheduler.checkDueTasksAt(now);
    QCOMPARE(fixture.delivered, QStringList{"unrestricted"});
    QCOMPARE(fixture.scheduler.tasks().first().nextTriggerAt, now.addSecs(60));
    fixture.scheduler.setUserBusyProvider([] { return true; });
    fixture.scheduler.checkDueTasksAt(now.addSecs(60));
    QCOMPARE(fixture.delivered.size(), 1);
    fixture.scheduler.setUserBusyProvider([] { return false; });
    fixture.scheduler.checkDueTasksAt(now.addSecs(120));
    QCOMPARE(fixture.delivered, QStringList({"unrestricted", "wait-for-idle"}));
}

void TestAgentScheduler::quietHoursResumeAtEight_data() {
    QTest::addColumn<QDateTime>("now");
    QTest::addColumn<QDateTime>("expected");
    QTest::newRow("morning") << clockAt("2026-10-05T07:55:00Z") << clockAt("2026-10-05T08:00:00Z");
    QTest::newRow("night") << clockAt("2026-10-05T23:45:00Z") << clockAt("2026-10-06T08:00:00Z");
}

void TestAgentScheduler::quietHoursResumeAtEight() {
    QFETCH(QDateTime, now);
    QFETCH(QDateTime, expected);
    Fixture fixture;
    QVERIFY(fixture.ready);
    auto task = taskAt("quiet", now);
    task.respectQuietHours = true;
    fixture.scheduler.m_tasks = {task};
    fixture.scheduler.checkDueTasksAt(now);
    QVERIFY(fixture.delivered.isEmpty());
    QCOMPARE(fixture.scheduler.tasks().first().nextTriggerAt, expected);
    fixture.scheduler.checkDueTasksAt(expected);
    QCOMPARE(fixture.delivered, QStringList{"quiet"});
}

void TestAgentScheduler::completedOnceReminderCanBeSnoozedAfterRestart() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("tasks.json");
    QStringList delivered;
    ToolRegistry registry;
    registry.registerTool(std::make_unique<BubbleTool>(&delivered));
    QString id;
    {
        AgentScheduler scheduler;
        scheduler.setStoragePath(path);
        scheduler.setToolRegistry(&registry);
        QVERIFY(scheduler.load());
        scheduler.start();
        const auto task = scheduler.createTask({{"type", "once_at"}, {"title", "water"},
                                                 {"delay_minutes", 1}, {"respect_quiet_hours", false}});
        QVERIFY(!task.id.isEmpty());
        id = task.id;
        scheduler.checkDueTasksAt(task.nextTriggerAt);
        QCOMPARE(delivered.size(), 1);
        QVERIFY(scheduler.tasks().isEmpty());
        QCOMPARE(scheduler.completedTasks().size(), 1);
        QVERIFY(!scheduler.completedTasks().first().enabled);
        QVERIFY(!scheduler.completedTasks().first().nextTriggerAt.isValid());
    }
    AgentScheduler restarted;
    restarted.setStoragePath(path);
    restarted.setToolRegistry(&registry);
    QVERIFY(restarted.load());
    QCOMPARE(restarted.completedTasks().size(), 1);
    restarted.start();
    QVERIFY(restarted.snoozeTask(id, 5));
    QVERIFY(restarted.completedTasks().isEmpty());
    QCOMPARE(restarted.tasks().size(), 1);
    const auto snoozed = restarted.tasks().first();
    QVERIFY(snoozed.enabled);
    QVERIFY(!snoozed.lastTriggeredAt.isValid());
    QVERIFY(snoozed.nextTriggerAt > QDateTime::currentDateTime());
    restarted.checkDueTasksAt(QDateTime::currentDateTime());
    QCOMPARE(delivered.size(), 1);
    QCOMPARE(restarted.tasks().size(), 1);
    restarted.checkDueTasksAt(snoozed.nextTriggerAt);
    QCOMPARE(delivered.size(), 2);
    QCOMPARE(restarted.completedTasks().size(), 1);
}

void TestAgentScheduler::completedHistoryRetainsOnlyTheLatestHundred() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const auto now = clockAt();
    for (int i = 0; i < 105; ++i) {
        fixture.scheduler.m_tasks.append(taskAt(QStringLiteral("done-%1").arg(i, 3, 10, QLatin1Char('0')),
                                                now.addSecs(i - 105)));
    }
    fixture.scheduler.checkDueTasksAt(now);
    QCOMPARE(fixture.delivered.size(), 105);
    QVERIFY(fixture.scheduler.tasks().isEmpty());
    const auto completed = fixture.scheduler.completedTasks();
    QCOMPARE(completed.size(), 100);
    QCOMPARE(completed.first().id, QStringLiteral("done-005"));
    QCOMPARE(completed.last().id, QStringLiteral("done-104"));
}

void TestAgentScheduler::stateSinkPreservesDisabledAndTerminalStates() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    auto disabled = taskAt("disabled", clockAt());
    disabled.enabled = false;
    auto completed = taskAt("completed", clockAt());
    completed.enabled = false;
    completed.lastTriggeredAt = clockAt();
    completed.nextTriggerAt = {};
    const auto cancelled = taskAt("cancelled", clockAt());
    fixture.scheduler.m_tasks = {disabled, cancelled};
    fixture.scheduler.m_completedTasks = {completed};
    fixture.scheduler.queueState(cancelled, "cancelled");
    QMap<QString, QString> statuses;
    fixture.scheduler.setStateSink([&statuses](const QJsonObject& state) {
        statuses.insert(state.value("id").toString(), state.value("status").toString());
        return true;
    });
    QCOMPARE(statuses.value("disabled"), QStringLiteral("disabled"));
    QCOMPARE(statuses.value("completed"), QStringLiteral("completed"));
    QCOMPARE(statuses.value("cancelled"), QStringLiteral("cancelled"));
}

void TestAgentScheduler::storageAvailabilityReportsExclusiveOwnership() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    QVERIFY(fixture.scheduler.storageAvailable());
    AgentScheduler second;
    second.setStoragePath(fixture.scheduler.storagePath());
    QVERIFY(!second.storageAvailable());
}

QTEST_GUILESS_MAIN(TestAgentScheduler)
#include "test_agent_scheduler.moc"
