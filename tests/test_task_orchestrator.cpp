#include "ai/chat/conversation_store.h"
#include "ai/task/task_orchestrator.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

namespace {
struct Harness {
    QTemporaryDir directory;
    TaskStore store;
    std::unique_ptr<TaskOrchestrator> runner;
    QList<TaskModelRequest> modelRequests;
    QList<TaskDependencies::ModelCompletion> models;
    QList<TaskToolCall> toolRequests;
    QList<TaskDependencies::ToolCompletion> tools;
    QList<std::function<void(TaskToolPermission)>> permissions;
    bool delayToolPreparation = false;
    QStringList cancelled;
    QJsonObject emotion{{"energy", .6}};
    QJsonArray schemas{QJsonObject{{"name", "web_search"}}, QJsonObject{{"name", "schedule_create"}},
        QJsonObject{{"name", "execute_whitelisted_command"}}};
    QStringList requiredSources;
    bool ready = false;
    Harness() {
        ready = store.open(directory.filePath("runtime.sqlite"),
                           "11111111-1111-4111-8111-111111111111");
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
        if (delayToolPreparation) {
            dependencies.prepareTool = [this](const TaskToolCall &, auto done) {
                permissions.append(std::move(done));
            };
        }
        dependencies.availableTools = [this] { return schemas; };
        dependencies.refreshContext = [this] { return QJsonObject{{"relationship", QJsonObject{{"trust", .7}}},
            {"emotion", emotion}, {"personality", QJsonObject{{"version", 999}}}}; };
        runner->setDependencies(std::move(dependencies));
    }
    bool start(TaskBudget budget = {}, const QString& goal = QStringLiteral("比较两份资料并验证结论")) {
        TaskStartRequest request;
        request.requiredSourceTools=requiredSources;
        request.originKind = OriginKind::LocalOwner;
        request.goal = goal;
        request.profileId = "11111111-1111-4111-8111-111111111111";
        request.subjectId = "owner";
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
  void ordinaryObservedMessageDoesNotMatchAnExternalWait();
  void taskRunner_realWaitProposal_usesHostReceiptAnchorAndReleasesForeground();
  void invalidWaitProposalCannotInventHostAnchors_data();
  void invalidWaitProposalCannotInventHostAnchors();
  void safeCheckpointPreservesReturnedPlanAndVerification();
  void importantReplyDuringToolPreparationStopsDispatch();
  void failedCheckpointStopsToolDispatch();
  void smallEmotionChangeDoesNotReselectAutonomousIntent();
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
  void pausedActivityAllowsAnotherAndKeepsOriginalCheckpoint();
  void lateToolReceiptArchivesAfterRunnerDestroyed();
  void lateReceiptStorageFailureDoesNotPauseReplacementActivity();
  void resumeRefreshesGroupParticipantsInRunnerAndModelScope();
};

void TestTaskOrchestrator::ordinaryObservedMessageDoesNotMatchAnExternalWait() {
  Harness h;
  QVERIFY(h.start());
  h.reply(decision());
  ConversationStore messages("11111111-1111-4111-8111-111111111111",
                             h.directory.filePath("runtime.sqlite"));
  HostConversationBinding binding;
  binding.channel = "controlled";
  binding.connectionKey = "one";
  binding.remoteConversationId = "unrelated";
  binding.kind = ConversationKind::Group;
  QVERIFY(messages.registerBinding(binding));
  IncomingMessage msg;
  msg.channel = binding.channel;
  msg.connectionKey = binding.connectionKey;
  msg.remoteConversationId = binding.remoteConversationId;
  msg.kind = binding.kind;
  msg.remoteMessageId = "new";
  msg.content = "A normal new group topic";
  msg.speakerIdentity = {"controlled", "one", "alice", {}};
  msg.occurredAt = QDateTime::currentDateTimeUtc();
  auto accepted = messages.acceptMessage(msg);
  QVERIFY(accepted);
  QVERIFY(!h.store.activityStore()->validateImportantReply(
      accepted.value().originId));
  QVERIFY(h.runner->isActive());
}
void TestTaskOrchestrator::
    taskRunner_realWaitProposal_usesHostReceiptAnchorAndReleasesForeground() {
  Harness h;
  QVERIFY(h.ready);
  QVERIFY(h.start());
  h.reply(decision());
  h.reply(nextTool("schedule_create", {{"job", "tracked"}}));
  QVERIFY(!h.tools.isEmpty());
  auto done = h.tools.takeFirst();
  TaskToolReply receipt;
  receipt.success = true;
  receipt.stage = "accepted";
  receipt.output = {{"stage", "accepted"},
                    {"tracking", QJsonObject{{"sourceId", "controlled-jobs"},
                                             {"remoteJobId", "job-42"}}}};
  done(receipt);
  QCoreApplication::processEvents();
  QCOMPARE(h.runner->currentTask().status, QString("Planning"));
  QVERIFY(!h.models.isEmpty());
  const auto callId = h.toolRequests.last().id;
  auto proposal = candidate();
  proposal.remove("candidate_answer");
  proposal["wait"] = QJsonObject{{"kind", "event"},
                                 {"afterStepId", callId},
                                 {"reason", "Wait for the tracked job"}};
  h.reply(proposal);
  QVERIFY(!h.runner->isActive());
  const auto saved = h.store.activityStore()->find(h.runner->currentTask().id);
  QVERIFY(saved);
  QCOMPARE(saved.value().state, ActivityState::WaitingExternal);
  QCOMPARE(saved.value().waitingCondition["sourceId"].toString(),
           QString("controlled-jobs"));
  QCOMPARE(saved.value().waitingCondition["remoteJobId"].toString(),
           QString("job-42"));
  QCOMPARE(saved.value().waitingCondition["callId"].toString(), callId);
}
void TestTaskOrchestrator::invalidWaitProposalCannotInventHostAnchors_data() {
  QTest::addColumn<QString>("problem");
  for (const auto &problem :
       {"foreign-step", "host-field", "long-reason", "alternative"})
    QTest::newRow(problem) << QString(problem);
}
void TestTaskOrchestrator::invalidWaitProposalCannotInventHostAnchors() {
  QFETCH(QString, problem);
  Harness h;
  QVERIFY(h.start());
  h.reply(decision());
  h.reply(nextTool("schedule_create", {{"job", "tracked"}}));
  auto done = h.tools.takeFirst();
  TaskToolReply receipt;
  receipt.success = true;
  receipt.stage = "accepted";
  receipt.output = {{"stage", "accepted"},
                    {"tracking", QJsonObject{{"sourceId", "jobs"},
                                             {"remoteJobId", "job-42"}}}};
  done(receipt);
  QCoreApplication::processEvents();
  auto proposal = candidate();
  proposal.remove("candidate_answer");
  QJsonObject wait{{"kind", "event"},
                   {"afterStepId", h.toolRequests.last().id},
                   {"reason", "Await actual job"}};
  if (problem == "foreign-step")
    wait["afterStepId"] = "another-activity-step";
  if (problem == "host-field")
    wait["resumeRule"] = "scheduler";
  if (problem == "long-reason")
    wait["reason"] = QString(257, 'x');
  proposal["wait"] = wait;
  if (problem == "alternative")
    proposal["candidate_answer"] = "done";
  h.reply(proposal);
  QCOMPARE(h.toolRequests.size(), 1);
  QVERIFY(
      h.store.activityStore()->find(h.runner->currentTask().id).value().state !=
      ActivityState::WaitingExternal);
}
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
    QVERIFY(!h.runner->resume());
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
    h.reply(decision("negotiate"));
    QVERIFY(h.start());
    QVERIFY(h.runner->currentTask().id != id);
    QCOMPARE(h.modelRequests.size(), 2);
}
void TestTaskOrchestrator::restartPreservesSnapshotAndBudgetAndDoesNotReplayAction() {
    Harness h; QVERIFY(h.start()); h.reply(decision());
    const QJsonObject args{{"title", "一次提醒"}, {"message", "test"}};
    h.reply(nextTool("schedule_create", args));
    const auto id = h.toolRequests.last().id;
    h.runner->shutdown();
    const qint64 consumed = h.runner->currentTask().activeMs;
    QTest::qWait(30); h.resetRunner(); QVERIFY(h.runner->recover());
    QCOMPARE(h.runner->currentTask().status, "Paused");
    QCOMPARE(h.runner->currentTask().activeMs, consumed);
    QCOMPARE(h.runner->currentTask().personalityVersion, 7);
    QCOMPARE(h.runner->currentTask().personalitySnapshot.value("version").toInt(), 7);
    QCOMPARE(h.runner->currentTask().toolCalls, 1);
    QVERIFY(!h.runner->resume()); QCOMPARE(h.toolRequests.size(), 1);
    QVERIFY(!h.store.activityStore()->resolveWaitCondition(
        h.runner->currentTask().id, id, "event"));
    QVERIFY(!h.store.activityStore()->resolveWaitCondition(
        h.runner->currentTask().id, id, "message"));
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

void TestTaskOrchestrator::
    pausedActivityAllowsAnotherAndKeepsOriginalCheckpoint() {
    Harness h;
    QVERIFY(h.start());
    h.reply(decision());
    h.reply(nextTool());
    h.tool();
    h.runner->pause();
    const auto first = h.runner->currentTask();
    QVERIFY(h.start({}, QStringLiteral("处理另一项已授权的任务")));
    QVERIFY(h.runner->currentTask().id != first.id);
    const auto saved = h.store.find(first.id);
    QVERIFY(saved.has_value());
    QCOMPARE(saved->status, QStringLiteral("Paused"));
    QCOMPARE(saved->steps, first.steps);
    QCOMPARE(saved->toolCalls, first.toolCalls);
}

void TestTaskOrchestrator::lateToolReceiptArchivesAfterRunnerDestroyed() {
    Harness h;
    QVERIFY(h.start());
    h.reply(decision());
    h.reply(nextTool("schedule_create", {{"title", "one action"}}));
    const auto taskId = h.runner->currentTask().id;
    auto completion = h.tools.takeFirst();
    h.runner->stop();
    h.runner.reset();
    completion({true, {{"receipt_id", "actual-remote-result"}}, {}, true});
    QTRY_COMPARE(h.store.find(taskId)
                     ->steps.last()
                     .toObject()
                     .value("status")
                     .toString(),
                 QStringLiteral("succeeded"));
    QCOMPARE(h.store.find(taskId)->status, QStringLiteral("Cancelled"));
    QCOMPARE(h.store.find(taskId)->toolCalls, 1);
    QCOMPARE(h.toolRequests.size(), 1);
    completion({true, {{"receipt_id", "actual-remote-result"}}, {}, true});
    QCoreApplication::processEvents();
    QCOMPARE(h.store.find(taskId)->toolCalls, 1);
}

void TestTaskOrchestrator::
    lateReceiptStorageFailureDoesNotPauseReplacementActivity() {
    Harness h;
    QVERIFY(h.start());
    h.reply(decision());
    h.reply(nextTool("schedule_create", {{"title", "first activity"}}));
    auto completeFirst = h.tools.takeFirst();
    h.runner->pause();
    QVERIFY(h.start({}, QStringLiteral("第二项活动")));
    const auto secondId = h.runner->currentTask().id;
    const auto connection = QUuid::createUuid().toString();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(h.directory.filePath("runtime.sqlite"));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(
            "CREATE TRIGGER reject_late_observation BEFORE UPDATE ON "
            "agent_action_calls BEGIN SELECT RAISE(ABORT,'archive "
            "unavailable'); END"));
    }
    QSqlDatabase::removeDatabase(connection);
    completeFirst({true, {{"receipt", "completed externally"}}, {}, true});
    QCoreApplication::processEvents();
    QCOMPARE(h.runner->currentTask().id, secondId);
    QVERIFY(h.runner->isActive());
    QVERIFY(h.store.find(secondId)->status != "Paused");
}

void TestTaskOrchestrator::
    resumeRefreshesGroupParticipantsInRunnerAndModelScope() {
    Harness h;
    const QString profile = "11111111-1111-4111-8111-111111111111";
    ConversationStore messages(profile, h.directory.filePath("runtime.sqlite"));
    QVERIFY(messages.initialize());
    HostConversationBinding binding;
    binding.channel = "chat";
    binding.connectionKey = "connection";
    binding.remoteConversationId = "group";
    binding.kind = ConversationKind::Group;
    QVERIFY(messages.registerBinding(binding));
    IncomingMessage message;
    message.channel = binding.channel;
    message.connectionKey = binding.connectionKey;
    message.remoteConversationId = binding.remoteConversationId;
    message.kind = ConversationKind::Group;
    message.remoteMessageId = "first";
    message.speakerIdentity = {binding.channel, binding.connectionKey,
                               "person-1", "First"};
    message.content = "Please read this";
    message.occurredAt = QDateTime::currentDateTimeUtc();
    const auto first = messages.acceptMessage(message);
    QVERIFY(first);
    TaskStartRequest request;
    request.profileId = profile;
    request.goal = "Read supplied group material";
    request.context = first.value().context;
    request.originKind = OriginKind::External;
    QVERIFY(h.runner->start(request));
    h.runner->pause();
    const auto oldRevision =
        h.runner->currentTask().context.conversationRevision;
    message.remoteMessageId = "second";
    message.speakerIdentity.remoteActorId = "person-2";
    message.speakerIdentity.displayName = "Second";
    QVERIFY(messages.acceptMessage(message));
    const auto current = messages.currentContext(first.value().conversationId);
    QVERIFY(current);
    QCOMPARE(current.value().participantPersonIds.size(), 2);
    QVERIFY(h.runner->resume());
    QCOMPARE(h.runner->currentTask().context.participantPersonIds,
             current.value().participantPersonIds);
    QVERIFY(h.runner->currentTask().context.conversationRevision > oldRevision);
    QCOMPARE(h.modelRequests.last().scope.participantPersonIds,
             current.value().participantPersonIds);
    QCOMPARE(
        h.store.find(h.runner->currentTask().id)->context.participantPersonIds,
        current.value().participantPersonIds);
}

void TestTaskOrchestrator::
    safeCheckpointPreservesReturnedPlanAndVerification() {
    for (bool duringVerification : {false, true}) {
        Harness h;
        QVERIFY(h.ready);
        ActivityIntent intent;
        intent.context.profileId = h.store.activityStore()->profileId();
        intent.origin = "self";
        intent.goal = "Read sources";
        auto started = h.store.activityStore()->startActivity(intent);
        QVERIFY(started);
        QVERIFY(h.runner->activate(started.value().activityId));
        h.reply(decision());
        if (!duringVerification) {
            h.runner->requestSafeCheckpoint("host-approved-reply");
            h.reply(nextTool());
            QCOMPARE(h.runner->currentTask().status, QString("Paused"));
            QVERIFY(h.toolRequests.isEmpty());
            QCOMPARE(h.runner->currentTask().plan, QString("查找来源并比较"));
            auto row =
                h.store.activityStore()->find(started.value().activityId);
            QVERIFY(row);
            QCOMPARE(row.value().pauseReason, QString("checkpoint"));
        } else {
            h.reply(nextTool());
            h.tool();
            h.reply(candidate());
            QCOMPARE(h.modelRequests.last().phase, QString("verify"));
            h.runner->requestSafeCheckpoint("host-approved-reply");
            h.reply(verification(true, h.runner->currentTask()
                                           .steps.last()
                                           .toObject()["id"]
                                           .toString()));
            QCOMPARE(h.runner->currentTask().status, QString("Completed"));
            QCOMPARE(h.runner->currentTask().toolCalls, 1);
        }
    }
}
void TestTaskOrchestrator::importantReplyDuringToolPreparationStopsDispatch() {
    Harness h;
    QVERIFY(h.ready);
    h.delayToolPreparation = true;
    h.resetRunner();
    ActivityIntent intent;
    intent.context.profileId = h.store.activityStore()->profileId();
    intent.origin = "self";
    intent.goal = "Read sources";
    const auto started = h.store.activityStore()->startActivity(intent);
    QVERIFY(started);
    QVERIFY(h.runner->activate(started.value().activityId));
    h.reply(decision());
    h.reply(nextTool());
    QCOMPARE(h.permissions.size(), 1);
    h.runner->requestSafeCheckpoint("reply-during-permission-load");
    h.permissions.takeFirst()({true, true});
    QCoreApplication::processEvents();
    QVERIFY(h.toolRequests.isEmpty());
    QVERIFY(!h.runner->isActive());
    QCOMPARE(h.runner->currentTask().status, QString("Paused"));
    QCOMPARE(h.runner->currentTask().plan, QString("查找来源并比较"));
    const auto saved =
        h.store.activityStore()->find(started.value().activityId);
    QVERIFY(saved);
    QCOMPARE(saved.value().pauseReason, QString("checkpoint"));
    QCOMPARE(saved.value().checkpoint["importantReplyRef"].toString(),
             QString("reply-during-permission-load"));
    QCOMPARE(saved.value().budget.toolCalls, 0);
}

void TestTaskOrchestrator::failedCheckpointStopsToolDispatch() {
    Harness h;
    QVERIFY(h.ready);
    ActivityIntent intent;
    intent.context.profileId = h.store.activityStore()->profileId();
    intent.origin = "self";
    intent.goal = "Read sources";
    const auto started = h.store.activityStore()->startActivity(intent);
    QVERIFY(started);
    QVERIFY(h.runner->activate(started.value().activityId));
    h.reply(decision());
    const QString connection = "reject-important-checkpoint";
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(h.directory.filePath("runtime.sqlite"));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(
            "CREATE TRIGGER reject_checkpoint BEFORE UPDATE ON "
            "agent_activities WHEN NEW.state='Paused' "
            "BEGIN SELECT RAISE(ABORT,'checkpoint unavailable'); END"));
    }
    QSqlDatabase::removeDatabase(connection);
    h.runner->requestSafeCheckpoint("important-reply");
    h.reply(nextTool());
    QVERIFY(h.toolRequests.isEmpty());
    QVERIFY(!h.runner->isActive());
    QCOMPARE(h.runner->currentTask().status, QString("Paused"));
    QVERIFY(h.runner->currentTask().reason.contains("checkpoint unavailable"));
    QCOMPARE(h.store.find(started.value().activityId)->toolCalls, 0);
}

void TestTaskOrchestrator::smallEmotionChangeDoesNotReselectAutonomousIntent() {
    Harness h;
    QVERIFY(h.ready);
    ActivityIntent intent;
    intent.context.profileId = h.store.activityStore()->profileId();
    intent.origin = "self";
    intent.goal = "Read sources";
    auto started = h.store.activityStore()->startActivity(intent);
    QVERIFY(started);
    QVERIFY(h.runner->activate(started.value().activityId));
    h.reply(decision());
    h.emotion = {{"energy", .61}};
    h.reply(nextTool());
    h.tool();
    QCOMPARE(h.modelRequests.last().phase, QString("plan"));
    QCOMPARE(h.runner->currentTask().id, started.value().activityId);
    QCOMPARE(h.runner->currentTask().decisions.size(), 1);
}

QTEST_GUILESS_MAIN(TestTaskOrchestrator)
#include "test_task_orchestrator.moc"
