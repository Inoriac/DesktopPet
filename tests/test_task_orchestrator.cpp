#include <QtTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <memory>
#include "ai/task/task_orchestrator.h"

namespace {
struct Harness {
    QTemporaryDir directory;
    TaskStore store;
    std::unique_ptr<TaskOrchestrator> runner;
    QList<TaskModelRequest> modelRequests;
    QList<TaskDependencies::ModelCompletion> models;
    QList<TaskToolCall> toolRequests;
    QList<TaskDependencies::ToolCompletion> tools;
    QStringList cancelled;
    QJsonObject emotion{{"energy", .6}};
    QJsonArray schemas{QJsonObject{{"name", "web_search"}}, QJsonObject{{"name", "schedule_create"}},
        QJsonObject{{"name", "execute_whitelisted_command"}}};
    QStringList requiredSources;
    bool ready = false;
    Harness() {
        ready = store.open(directory.filePath("runtime.sqlite"));
        resetRunner();
    }
    ~Harness() { runner.reset(); }
    void resetRunner() {
        runner.reset();
        models.clear(); tools.clear();
        runner = std::make_unique<TaskOrchestrator>(&store);
        TaskDependencies dependencies;
        dependencies.requestModel = [this](const TaskModelRequest& request, auto done) {
            modelRequests.append(request); models.append(std::move(done));
        };
        dependencies.executeTool = [this](const TaskToolCall& call, auto done) {
            // The callback sees the dispatch intent already durably committed.
            const auto stored = store.find(runner->currentTask().id);
            if (!stored || stored->toolCalls != runner->currentTask().toolCalls) ready = false;
            toolRequests.append(call); tools.append(std::move(done));
        };
        dependencies.cancelModel = dependencies.cancelTool = [this](const QString& id) { cancelled.append(id); };
        dependencies.toolMetadata = [](const QString& name) {
            if (name=="web_search") return HostToolMetadata::safeRead();
            if (name=="schedule_create") return HostToolMetadata::localAction();
            return HostToolMetadata{};
        };
        dependencies.toolAllowed = [](const QString&, const QJsonObject&) { return true; };
        dependencies.availableTools = [this] { return schemas; };
        dependencies.refreshContext = [this] { return QJsonObject{{"relationship", QJsonObject{{"trust", .7}}},
            {"emotion", emotion}, {"personality", QJsonObject{{"version", 999}}}}; };
        runner->setDependencies(std::move(dependencies));
    }
    bool start(TaskBudget budget = {}, const QString& goal = QStringLiteral("比较两份资料并验证结论")) {
        TaskStartRequest request;
        request.requiredSourceTools=requiredSources;
        request.goal = goal; request.profileId = "pet"; request.subjectId = "owner";
        request.personalityVersion = 7; request.personalitySnapshot = {{"version", 7}, {"style", "patient"}};
        request.emotionSnapshot = {{"energy", .9}}; request.budget = budget;
        return runner->start(request);
    }
    void reply(const QJsonObject& output, bool success = true) {
        Q_ASSERT(!models.isEmpty()); auto done = models.takeFirst(); done({success, output, success ? "" : "offline"});
        QCoreApplication::processEvents();
    }
    void tool(bool success = true) {
        Q_ASSERT(!tools.isEmpty()); auto done = tools.takeFirst(); done({success, {{"source", "https://example.org"}}, success ? "" : "offline"});
        QCoreApplication::processEvents();
    }
};
QJsonObject decision(const QString& value = "accept") {
    return {{"decision", value}, {"reason", "考虑了当前精力与任务范围"}, {"message", "我们继续。"}};
}
QJsonObject nextTool(const QString& name = "web_search", const QJsonObject& args = {{"query", "sources"}}) {
    return {{"required_source_tools",QJsonArray{}},{"plan", "查找来源并比较"}, {"completion_criteria", "两项主张均有可信来源且无矛盾"},
        {"tool", QJsonObject{{"name", name}, {"arguments", args}}}};
}
QJsonObject candidate() {
    return {{"required_source_tools",QJsonArray{}},{"plan", "已有候选结果，等待独立验证"}, {"completion_criteria", "所有主张均有来源且无矛盾"}, {"candidate_answer", "候选结论"}};
}
QJsonObject verification(bool complete, const QString& reference = "user_input") {
    return {{"complete", complete}, {"supported", complete}, {"contradictions", QJsonArray{}},
        {"unresolved", complete ? QJsonArray{} : QJsonArray{"source conflict"}},
        {"evidence", QJsonArray{reference}}, {"result", "已核实的结论及来源。"}, {"reason", "检查来源与完成条件"}};
}
}

class TestTaskOrchestrator : public QObject {
    Q_OBJECT
private slots:
    void stopCallingToolsDoesNotCompleteUntilVerification();
    void unsafeToolsCannotBeEnabledByModelOrHook();
    void failedToolCallsConsumeBudget();
    void verificationCannotExceedReplanBudget();
    void agencyFailureRetriesOnceAndIsNotPersonalityRefusal();
    void agencyChoices_data();
    void agencyChoices();
    void cancellationRejectsLateModelAndToolCallbacks();
    void timeoutAndActiveBudgetActuallyPause();
    void secondRequestPreservesUnfinishedTask();
    void restartPreservesSnapshotAndBudgetAndDoesNotReplayAction();
    void terminalCheckpointAndExperienceIntentAreAtomic();
    void completedExperienceIsIdempotent();
    void falseSourceCannotPassVerification();
    void checkpointSubjectivityHasCooldownAndDedup();
    void significantEmotionChangeTriggersCheckpoint();
    void externalTaskNeedsCapabilityBeforeAgencyAndRealEvidenceToComplete();
    void clarificationIsNotOutcomeFeedback();
    void cancelledCandidateIsLabeledAsUnfinished();
};

void TestTaskOrchestrator::stopCallingToolsDoesNotCompleteUntilVerification() {
    Harness h; QVERIFY(h.ready); QVERIFY(h.start());
    QCOMPARE(h.modelRequests.last().role, "agency_decision");
    h.reply(decision()); h.reply(candidate());
    QCOMPARE(h.runner->currentTask().status, "Verifying");
    QVERIFY(h.store.pendingExperiences().isEmpty());
    h.reply(verification(false));
    QCOMPARE(h.runner->currentTask().status, "Replanning");
    h.reply(nextTool()); QVERIFY(h.ready); h.tool();
    const QString source = h.toolRequests.last().id;
    h.reply(candidate()); h.reply(verification(true, source));
    QCOMPARE(h.runner->currentTask().status, "Completed");
    QCOMPARE(h.runner->currentTask().toolCalls, 1);
    QCOMPARE(h.runner->currentTask().replans, 1);
    QCOMPARE(h.store.pendingExperiences().size(), 1);
}
void TestTaskOrchestrator::unsafeToolsCannotBeEnabledByModelOrHook() {
    Harness h; QVERIFY(h.start()); h.reply(decision());
    const auto exposed = h.modelRequests.last().context.value("available_tools").toArray();
    QCOMPARE(exposed.size(), 2);
    h.reply(nextTool("execute_whitelisted_command"));
    QCOMPARE(h.runner->currentTask().status, "Paused");
    QVERIFY(h.toolRequests.isEmpty());
    QVERIFY(!h.runner->allowedTool("write_text_file"));
    QVERIFY(!h.runner->allowedTool("desktop_click"));
    QVERIFY(!h.runner->allowedTool("run_script"));
}
void TestTaskOrchestrator::failedToolCallsConsumeBudget() {
    Harness h; TaskBudget budget; budget.maxToolCalls = 2;
    QVERIFY(h.start(budget)); h.reply(decision());
    h.reply(nextTool()); h.tool(false); h.reply(nextTool()); h.tool(false);
    QCOMPARE(h.modelRequests.last().phase, "checkpoint");
    h.reply(decision("continue")); h.reply(nextTool());
    QCOMPARE(h.runner->currentTask().status, "Paused");
    QCOMPARE(h.runner->currentTask().toolCalls, 2);
    QCOMPARE(h.toolRequests.size(), 2);
    QVERIFY(h.runner->resume());
    h.reply(nextTool());
    QCOMPARE(h.runner->currentTask().toolCalls, 2);
    QCOMPARE(h.runner->currentTask().status, "Paused");
}
void TestTaskOrchestrator::verificationCannotExceedReplanBudget() {
    Harness h; QVERIFY(h.start()); h.reply(decision());
    for (int i = 0; i < 3; ++i) { h.reply(candidate()); h.reply(verification(false)); }
    QCOMPARE(h.runner->currentTask().status, "Paused");
    QCOMPARE(h.runner->currentTask().replans, 2);
    QVERIFY(h.store.pendingExperiences().isEmpty());
}
void TestTaskOrchestrator::agencyFailureRetriesOnceAndIsNotPersonalityRefusal() {
    Harness h; QVERIFY(h.start()); h.reply({}, false); h.reply({}, false);
    QCOMPARE(h.modelRequests.size(), 2);
    QCOMPARE(h.runner->currentTask().status, "Paused");
    QVERIFY(h.runner->currentTask().decisions.isEmpty());
    QVERIFY(h.store.pendingExperiences().isEmpty());
}
void TestTaskOrchestrator::agencyChoices_data() {
    QTest::addColumn<QString>("choice"); QTest::addColumn<QString>("status");
    QTest::newRow("negotiate") << QString("negotiate") << QString("WaitingForUser");
    QTest::newRow("defer") << QString("defer") << QString("WaitingForUser");
    QTest::newRow("decline") << QString("decline") << QString("Declined");
}
void TestTaskOrchestrator::agencyChoices() {
    QFETCH(QString, choice); QFETCH(QString, status);
    Harness h; QVERIFY(h.start()); h.reply(decision(choice));
    QCOMPARE(h.runner->currentTask().status, status); QVERIFY(!h.runner->isActive());
    QCOMPARE(h.runner->currentTask().decisions.first().toObject().value("personality_version").toInteger(), 7);
    QCOMPARE(h.store.pendingExperiences().size(), choice == "decline" ? 1 : 0);
}
void TestTaskOrchestrator::cancellationRejectsLateModelAndToolCallbacks() {
    Harness h; QVERIFY(h.start()); auto lateModel = h.models.takeFirst();
    h.runner->stop(); lateModel({true, decision(), {}}); QCoreApplication::processEvents();
    QCOMPARE(h.runner->currentTask().status, "Cancelled");
    QCOMPARE(h.modelRequests.size(), 1);
    QVERIFY(h.start()); h.reply(decision()); h.reply(nextTool());
    auto lateTool = h.tools.takeFirst(); h.runner->stop();
    lateTool({true, {{"result", "late"}}, {}}); QCoreApplication::processEvents();
    QCOMPARE(h.runner->currentTask().status, "Cancelled");
    QCOMPARE(h.runner->currentTask().steps.first().toObject().value("status").toString(), "interrupted");
    QCOMPARE(h.modelRequests.size(), 3);
}
void TestTaskOrchestrator::timeoutAndActiveBudgetActuallyPause() {
    Harness h; TaskBudget budget; budget.modelTimeoutMs = 20; budget.maxActiveMs = 1000;
    QVERIFY(h.start(budget));
    QTRY_COMPARE_WITH_TIMEOUT(h.runner->currentTask().status, QString("Paused"), 1000);
    QCOMPARE(h.modelRequests.size(), 2);
    QVERIFY(h.runner->currentTask().activeMs >= 20);
    h.runner->stop();
    budget.modelTimeoutMs = 1000; budget.maxActiveMs = 30;
    QVERIFY(h.start(budget));
    QTRY_COMPARE_WITH_TIMEOUT(h.runner->currentTask().status, QString("Paused"), 1000);
    QVERIFY(h.runner->currentTask().activeMs >= 30);
    const qint64 consumed = h.runner->currentTask().activeMs;
    QTest::qWait(40); QCOMPARE(h.runner->currentTask().activeMs, consumed);
    QVERIFY(!h.runner->resume());
    QVERIFY(h.runner->resume({}, {0, 0, 1000}));
    QVERIFY(h.runner->currentTask().activeMs >= consumed);
}
void TestTaskOrchestrator::secondRequestPreservesUnfinishedTask() {
    Harness h; QVERIFY(h.start()); const auto id = h.runner->currentTask().id;
    QVERIFY(!h.start()); QCOMPARE(h.runner->currentTask().id, id);
    h.reply(decision("negotiate")); QVERIFY(!h.start()); QCOMPARE(h.runner->currentTask().id, id);
    QCOMPARE(h.modelRequests.size(), 1);
}
void TestTaskOrchestrator::restartPreservesSnapshotAndBudgetAndDoesNotReplayAction() {
    Harness h; QVERIFY(h.start()); h.reply(decision());
    const QJsonObject args{{"title", "一次提醒"}, {"message", "test"}};
    h.reply(nextTool("schedule_create", args));
    const auto id = h.toolRequests.last().id;
    h.runner->shutdown();
    const qint64 consumed = h.runner->currentTask().activeMs;
    QTest::qWait(30); h.resetRunner(); QVERIFY(h.runner->recover());
    QCOMPARE(h.runner->currentTask().status, "WaitingForUser");
    QCOMPARE(h.runner->currentTask().activeMs, consumed);
    QCOMPARE(h.runner->currentTask().personalityVersion, 7);
    QCOMPARE(h.runner->currentTask().personalitySnapshot.value("version").toInt(), 7);
    QCOMPARE(h.runner->currentTask().toolCalls, 1);
    QVERIFY(!h.runner->resume()); QCOMPARE(h.toolRequests.size(), 1);
    QVERIFY(h.runner->resolveUnknownCall(id, true, {{"schedule_id", "existing-reminder"}}));
    QVERIFY(h.runner->resume());
    QVERIFY(h.modelRequests.last().context.value("needs_revalidation").toBool());
    h.reply(nextTool("schedule_create", args));
    QCOMPARE(h.toolRequests.size(), 1); QCOMPARE(h.runner->currentTask().status, "Verifying");
    h.reply(verification(true, id)); QCOMPARE(h.runner->currentTask().status, "Completed");
    QCOMPARE(h.store.pendingExperiences().size(), 1);
}
void TestTaskOrchestrator::terminalCheckpointAndExperienceIntentAreAtomic() {
    QTemporaryDir dir; TaskStore store; const auto path = dir.filePath("runtime.sqlite"); QVERIFY(store.open(path));
    TaskRecord task; task.id = "atomic"; task.goal = "test"; task.updatedAt = "now"; QVERIFY(store.save(task));
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "task-atomic-test"); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec("CREATE TRIGGER reject_experience BEFORE INSERT ON complex_task_experience_outbox BEGIN SELECT RAISE(ABORT, 'injected failure'); END"));
        task.status = "Completed"; QVERIFY(!store.save(task, true));
        QCOMPARE(store.find(task.id)->status, "Received"); QVERIFY(store.pendingExperiences().isEmpty());
        QVERIFY(query.exec("DROP TRIGGER reject_experience")); db.close();
    }
    QSqlDatabase::removeDatabase("task-atomic-test");
    QVERIFY(store.save(task, true)); QCOMPARE(store.find(task.id)->status, "Completed");
    QCOMPARE(store.pendingExperiences().size(), 1);
}
void TestTaskOrchestrator::completedExperienceIsIdempotent() {
    Harness h; QVERIFY(h.start()); h.reply(decision()); h.reply(candidate()); h.reply(verification(true));
    const auto task = h.runner->currentTask();
    QVERIFY(h.store.save(task, true)); h.runner->stop();
    QCOMPARE(h.store.pendingExperiences().size(), 1);
    QCOMPARE(h.store.pendingExperiences().first().toObject().value("feedback_status").toString(), "unknown");
    QVERIFY(h.store.acknowledgeExperience(task.id)); QVERIFY(h.store.pendingExperiences().isEmpty());
    QVERIFY(h.store.save(task, true)); QVERIFY(h.store.pendingExperiences().isEmpty());
}
void TestTaskOrchestrator::falseSourceCannotPassVerification() {
    Harness h; QVERIFY(h.start()); h.reply(decision()); h.reply(candidate());
    h.reply(verification(true, "invented-source"));
    QCOMPARE(h.runner->currentTask().status, "Replanning");
    QVERIFY(h.store.pendingExperiences().isEmpty());
}
void TestTaskOrchestrator::checkpointSubjectivityHasCooldownAndDedup() {
    Harness h; QVERIFY(h.start()); h.reply(decision());
    h.runner->notifyAgencyTrigger("emotion-1", "精力显著下降");
    h.reply(nextTool()); h.tool(); QCOMPARE(h.modelRequests.last().phase, "checkpoint");
    h.reply(decision("continue"));
    h.runner->notifyAgencyTrigger("emotion-1", "精力显著下降");
    h.reply(nextTool()); h.tool(); QCOMPARE(h.modelRequests.last().phase, "plan");
    h.runner->notifyAgencyTrigger("emotion-2", "再次变化");
    h.reply(nextTool()); h.tool(); QCOMPARE(h.modelRequests.last().phase, "plan");
    QCOMPARE(h.runner->currentTask().decisions.size(), 2);
}

void TestTaskOrchestrator::significantEmotionChangeTriggersCheckpoint() {
    Harness h; QVERIFY(h.start()); h.reply(decision()); h.reply(nextTool());
    h.emotion = {{"energy", .1}};
    h.tool();
    QCOMPARE(h.modelRequests.last().phase, "checkpoint");
    QVERIFY(h.modelRequests.last().context.value("trigger").toString().startsWith("emotion:"));
    h.reply(decision("pause"));
    QCOMPARE(h.runner->currentTask().status, "Paused");
    QCOMPARE(h.runner->currentTask().personalityVersion, 7);
}

void TestTaskOrchestrator::externalTaskNeedsCapabilityBeforeAgencyAndRealEvidenceToComplete() {
    Harness h;
    h.schemas = {};
    h.requiredSources={"web_search"};
    QVERIFY(h.start({}, QStringLiteral("检索最新旅行建议")));
    QCOMPARE(h.runner->currentTask().status, "Paused");
    QVERIFY(h.modelRequests.isEmpty());
    h.schemas = {QJsonObject{{"name", "web_search"}}};
    QVERIFY(h.runner->resume()); h.reply(decision()); h.reply(candidate());
    h.reply(verification(true, "user_input"));
    QCOMPARE(h.runner->currentTask().status, "Replanning");
    h.reply(nextTool()); h.tool(); h.reply(candidate());
    h.reply(verification(true, h.toolRequests.first().id));
    QCOMPARE(h.runner->currentTask().status, "Completed");
}

void TestTaskOrchestrator::clarificationIsNotOutcomeFeedback() {
    Harness h; QVERIFY(h.start()); h.reply(decision("negotiate"));
    QVERIFY(h.runner->resume(QStringLiteral("只考虑杭州，明天出发")));
    QVERIFY(h.modelRequests.last().context.value("requirement_updates").toString().contains(QStringLiteral("杭州")));
    h.reply(decision()); h.reply(candidate()); h.reply(verification(true));
    const auto experience = h.store.pendingExperiences().first().toObject();
    QCOMPARE(experience.value("feedback_status").toString(), "unknown");
    QVERIFY(experience.value("user_feedback").toString().isEmpty());
    QVERIFY(experience.value("requirement_updates").toString().contains(QStringLiteral("杭州")));
}

void TestTaskOrchestrator::cancelledCandidateIsLabeledAsUnfinished() {
    Harness h;
    QSignalSpy messages(h.runner.get(), &TaskOrchestrator::userMessage);
    QVERIFY(h.start()); h.reply(decision()); h.reply(candidate());
    QCOMPARE(h.runner->currentTask().status, "Verifying");
    h.runner->stop(QStringLiteral("用户停止了任务。"));
    QCOMPARE(h.runner->currentTask().status, "Cancelled");
    const QString message = messages.last().first().toString();
    QVERIFY(message.contains(QStringLiteral("用户停止了任务。")));
    QVERIFY(message.contains(QStringLiteral("未标记为完成结果")));
    QVERIFY(message.contains(QStringLiteral("候选结论")));
    QCOMPARE(h.store.pendingExperiences().first().toObject().value("confidence").toString(), "limited");
}

QTEST_GUILESS_MAIN(TestTaskOrchestrator)
#include "test_task_orchestrator.moc"
