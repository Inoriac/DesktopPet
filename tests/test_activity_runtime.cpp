#include "ai/activity/activity_runtime.h"
#include "ai/activity/activity_store.h"
#include "ai/chat/conversation_store.h"
#include "ai/plugin/plugin_runtime.h"
#include "ai/reflection/sleep_session_repository.h"
#include "ai/task/task_store.h"
#include <QFile>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtTest>

namespace {
const QString profile = "11111111-1111-4111-8111-111111111111";
ActivityIntent intent(const QString &goal = "read") {
    ActivityIntent i;
    i.goal = goal;
    i.context.profileId = profile;
    i.context.originKind = OriginKind::Internal;
    return i;
}
ActivityTransition transition(const ActivitySnapshot &a, ActivityState state) {
    ActivityTransition t;
    t.profileId = profile;
    t.activityId = a.activityId;
    t.expectedRevision = a.revision;
    t.targetState = state;
    return t;
}
ResumeActivityRequest resume(const ActivitySnapshot &a) {
    ResumeActivityRequest r;
    r.profileId = profile;
    r.activityId = a.activityId;
    r.expectedRevision = a.revision;
    r.resumeActor = ResumeActor::User;
    return r;
}
ActionPreparation action(const ActivitySnapshot &a, bool read = true) {
    ActionPreparation p;
    p.profileId = profile;
    p.activityId = a.activityId;
    p.expectedRevision = a.revision;
    p.generation = a.generation;
    p.logicalStepId = "step-1";
    p.capabilityRef = {"builtin", "probe", "1", "invoke"};
    p.arguments = {{"query", "news"}};
    p.readOnly = read;
    return p;
}
QVariant sql(const QString &path, const QString &statement) {
    const auto name = QUuid::createUuid().toString();
    QVariant value;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(statement)) {
                if (q.next())
                    value = q.value(0);
                else if (!q.isSelect())
                    value = true;
            } else
                qWarning() << q.lastError();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return value;
}
QString sendMessage(ConversationStore &messages, const AcceptedMessage &input) {
    ContextRequestV2 request;
    request.context = input.context;
    request.replyTarget = input.replyTarget;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.generation = 1;
    auto prepared = messages.prepareContext(request);
    if (!prepared)
        return {};
    RoutedResponse response;
    response.context = prepared.value().context;
    response.replyTarget = prepared.value().replyTarget;
    response.requestId = request.requestId;
    response.generation = 1;
    response.content = "sent";
    if (!messages.publishResponse(response))
        return {};
    DeliveryReceipt receipt;
    receipt.requestId = request.requestId;
    receipt.generation = 1;
    receipt.replyTarget = response.replyTarget;
    receipt.outcome = DeliveryOutcome::Completed;
    receipt.remoteMessageId = "sent-1";
    receipt.content = "sent";
    receipt.occurredAt = QDateTime::currentDateTimeUtc();
    if (!messages.recordDeliveryReceipt(receipt))
        return {};
    auto history = messages.loadHistory(input.conversationId);
    if (!history || history.value().isEmpty())
        return {};
    return history.value().last().id;
}
Result<ActionSnapshot, DomainError>
dispatch(ActivityStore &store, const ActivitySnapshot &a, bool read = true) {
    auto p = store.prepareAction(action(a, read));
    if (!p)
        return p;
    return store.markActionDispatched(
        {profile, a.activityId, p.value().callId, a.revision, a.generation});
}
ActionObservation observed(const ActivitySnapshot &a,
                           const ActionSnapshot &call,
                           ActionOutcome outcome = ActionOutcome::Succeeded) {
    ActionObservation o;
    o.profileId = profile;
    o.activityId = a.activityId;
    o.callId = call.callId;
    o.dispatchGeneration = call.generation;
    o.outcome = outcome;
    o.observedAt = QDateTime::currentDateTimeUtc();
    o.receipt = {{"remoteMessageId", "sent-1"}};
    return o;
}
} // namespace
class TestActivityRuntime : public QObject {
    Q_OBJECT
  private slots:
    void startActivity_multistepRead_holdsForegroundAcrossSteps() {
        QTemporaryDir dir;
        ActivityStore store(profile, dir.filePath("runtime.db"));
        QVERIFY(store.initialize());
        auto a = store.startActivity(intent());
        QVERIFY(a);
        QCOMPARE(a.value().state, ActivityState::Running);
        auto t = transition(a.value(), ActivityState::Running);
        t.executionPhase = "planning";
        t.checkpoint = {{"page", 1}};
        auto next = store.transitionActivity(t);
        QVERIFY(next);
        t = transition(next.value(), ActivityState::Running);
        t.executionPhase = "observing";
        t.checkpoint = {{"page", 2}};
        next = store.transitionActivity(t);
        QVERIFY(next);
        QCOMPARE(next.value().checkpoint["page"].toInt(), 2);
        auto other = store.startActivity(intent("chat"));
        QVERIFY(other);
        QCOMPARE(other.value().state, ActivityState::Ready);
        QCOMPARE(store.list({ActivityState::Running}).value().size(), 1);
        auto stale = store.transitionActivity(
            transition(a.value(), ActivityState::Paused));
        QVERIFY(!stale);
        QCOMPARE(stale.error().code, "STALE_REVISION");
    }
    void transitionActivity_sentReceiptWaiting_releasesForeground() {
        QTemporaryDir dir;
        const auto path = dir.filePath("runtime.db");
        ActivityStore store(profile, path);
        QVERIFY(store.initialize());
        ConversationStore messages(profile, path);
        QVERIFY(messages.initialize());
        HostConversationBinding binding;
        binding.channel = "chat";
        binding.connectionKey = "conn";
        binding.remoteConversationId = "room";
        QVERIFY(messages.registerBinding(binding));
        IncomingMessage message;
        message.channel = "chat";
        message.connectionKey = "conn";
        message.remoteConversationId = "room";
        message.remoteMessageId = "first";
        message.speakerIdentity = {"chat", "conn", "friend", "Friend"};
        message.content = "hello";
        message.occurredAt = QDateTime::currentDateTimeUtc();
        auto first = messages.acceptMessage(message);
        QVERIFY(first);
        auto i = intent("chat");
        i.context = first.value().context;
        auto a = store.startActivity(i);
        QVERIFY(a);
        auto t = transition(a.value(), ActivityState::WaitingExternal);
        t.waitingCondition = {{"kind", "message"},
                              {"conversationId", first.value().conversationId},
                              {"afterMessageId", "missing"},
                              {"resumeRule", "event"}};
        QVERIFY(!store.transitionActivity(t));
        auto call = dispatch(store, a.value(), false);
        QVERIFY(call);
        QVERIFY(store.observeAction(observed(a.value(), call.value())));
        a = store.find(a.value().activityId);
        QVERIFY(a);
        t.expectedRevision = a.value().revision;
        t.waitingCondition["callId"] = call.value().callId;
        QVERIFY(!store.transitionActivity(t));
        const auto anchor = sendMessage(messages, first.value());
        QVERIFY(!anchor.isEmpty());
        t.waitingCondition["afterMessageId"] = anchor;
        auto waiting = store.transitionActivity(t);
        QVERIFY(waiting);
        QCOMPARE(waiting.value().state, ActivityState::WaitingExternal);
        auto read = store.startActivity(intent());
        QVERIFY(read);
        QCOMPARE(read.value().state, ActivityState::Running);
    }
    void resumeActivity_matchingReply_resumesChatThenReadingCheckpoint() {
        QTemporaryDir dir;
        const auto path = dir.filePath("runtime.db");
        ActivityStore store(profile, path);
        QVERIFY(store.initialize());
        ConversationStore messages(profile, path);
        QVERIFY(messages.initialize());
        HostConversationBinding binding;
        binding.channel = "chat";
        binding.connectionKey = "conn";
        binding.remoteConversationId = "room";
        QVERIFY(messages.registerBinding(binding));
        IncomingMessage message;
        message.channel = "chat";
        message.connectionKey = "conn";
        message.remoteConversationId = "room";
        message.remoteMessageId = "first";
        message.speakerIdentity = {"chat", "conn", "friend", "Friend"};
        message.content = "hello";
        message.occurredAt = QDateTime::currentDateTimeUtc();
        auto first = messages.acceptMessage(message);
        QVERIFY(first);
        auto i = intent("chat");
        i.context = first.value().context;
        auto chat = store.startActivity(i);
        QVERIFY(chat);
        auto call = dispatch(store, chat.value(), false);
        QVERIFY(call);
        auto receipt = observed(chat.value(), call.value());
        receipt.receipt["conversationId"] = first.value().conversationId;
        QVERIFY(store.observeAction(receipt));
        const auto anchor = sendMessage(messages, first.value());
        QVERIFY(!anchor.isEmpty());
        chat = store.find(chat.value().activityId);
        auto t = transition(chat.value(), ActivityState::WaitingExternal);
        t.waitingCondition = {{"kind", "message"},
                              {"conversationId", first.value().conversationId},
                              {"afterMessageId", anchor},
                              {"afterRevision", first.value().revision},
                              {"callId", call.value().callId},
                              {"resumeRule", "event"}};
        chat = store.transitionActivity(t);
        QVERIFY(chat);
        auto reading = store.startActivity(intent());
        QVERIFY(reading);
        t = transition(reading.value(), ActivityState::Running);
        t.executionPhase = "checkpoint";
        t.checkpoint = {{"page", 12}};
        reading = store.transitionActivity(t);
        QVERIFY(reading);
        auto r = resume(chat.value());
        r.resumeActor = ResumeActor::Event;
        r.triggerRef = "missing";
        r.expectedForegroundId = reading.value().activityId;
        r.expectedForegroundRevision = reading.value().revision;
        r.foregroundActiveMs = 314;
        QVERIFY(!store.resumeActivity(r));
        QCOMPARE(store.find(reading.value().activityId).value().budget.activeMs,
                 0);
        message.remoteMessageId = "reply";
        message.content = "new reply";
        auto reply = messages.acceptMessage(message);
        QVERIFY(reply);
        r.triggerRef = reply.value().originId;
        auto resumed = store.resumeActivity(r);
        QVERIFY(resumed);
        QCOMPARE(resumed.value().state, ActivityState::Running);
        QCOMPARE(store.find(reading.value().activityId).value().state,
                 ActivityState::Paused);
        QCOMPARE(store.find(reading.value().activityId).value().budget.activeMs,
                 314);
        t = transition(resumed.value(), ActivityState::Cancelled);
        QVERIFY(store.transitionActivity(t));
        reading = store.find(reading.value().activityId);
        auto back = store.resumeActivity(resume(reading.value()));
        QVERIFY(back);
        QCOMPARE(back.value().checkpoint["page"].toInt(), 12);
        QCOMPARE(back.value().budget.activeMs, 314);
    }
    void observeAction_verifiedLateReceipt_settlesOnceWithoutResend() {
        QTemporaryDir dir;
        ActivityStore store(profile, dir.filePath("runtime.db"));
        QVERIFY(store.initialize());
        auto a = store.startActivity(intent());
        QVERIFY(a);
        auto call = dispatch(store, a.value(), false);
        QVERIFY(call);
        QVERIFY(store.recoverAfterRestart());
        auto recovered = store.find(a.value().activityId);
        QVERIFY(recovered);
        QCOMPARE(store.findAction(call.value().callId).value().status,
                 "unknown");
        QCOMPARE(recovered.value().budget.toolCalls, 1);
        QVERIFY(!store.resumeActivity(resume(recovered.value())));
        auto o = observed(a.value(), call.value());
        QVERIFY(store.observeAction(o));
        QVERIFY(store.observeAction(o));
        recovered = store.find(a.value().activityId);
        QCOMPARE(recovered.value().budget.toolCalls, 1);
        QVERIFY(store.resumeActivity(resume(recovered.value())));
        auto active = store.find(a.value().activityId);
        auto repeat = store.prepareAction(action(active.value(), false));
        QVERIFY(repeat);
        QCOMPARE(repeat.value().callId, call.value().callId);
        QCOMPARE(repeat.value().status, "succeeded");
        QVERIFY(!store.markActionDispatched(
            {profile, active.value().activityId, call.value().callId,
             active.value().revision, active.value().generation}));
    }
    void resumeActivity_restart_keepsBudgetsAndRequiresValidResumeAuthority() {
        QTemporaryDir dir;
        const auto path = dir.filePath("runtime.db");
        ActivityStore store(profile, path);
        QVERIFY(store.initialize());
        auto a = store.startActivity(intent());
        QVERIFY(a);
        auto t = transition(a.value(), ActivityState::Paused);
        t.reason = "user";
        a = store.transitionActivity(t);
        QVERIFY(a);
        auto r = resume(a.value());
        r.resumeActor = ResumeActor::Scheduler;
        QVERIFY(!store.resumeActivity(r));
        QVERIFY(store.recoverAfterRestart());
        a = store.find(a.value().activityId);
        r = resume(a.value());
        r.resumeActor = ResumeActor::Event;
        QVERIFY(!store.resumeActivity(r));
        QVERIFY(store.resumeActivity(resume(a.value())));
        QCOMPARE(sql(path, "PRAGMA user_version").toInt(), 3);
        SleepSessionRepository sleep;
        QVERIFY(sleep.open(path));
        QCOMPARE(sql(path, "PRAGMA user_version").toInt(), 3);
    }
    void transitionActivity_emptyCheckCompleted_modelFailureNotCompleted() {
        QTemporaryDir dir;
        ActivityStore store(profile, dir.filePath("runtime.db"));
        QVERIFY(store.initialize());
        auto a = store.startActivity(intent("check"));
        QVERIFY(a);
        auto t = transition(a.value(), ActivityState::Completed);
        t.result = "no messages";
        t.emptyCheck = true;
        QVERIFY(!store.transitionActivity(t));
        auto call = dispatch(store, a.value());
        QVERIFY(call);
        auto o = observed(a.value(), call.value());
        CapabilityOutcome output;
        output.stage = "completed";
        output.executed = true;
        output.data = QJsonObject{{"items", QJsonArray{}}, {"empty", true}};
        o.receipt = output.toJson();
        QVERIFY(store.observeAction(o));
        a = store.find(a.value().activityId);
        t.expectedRevision = a.value().revision;
        t.evidenceRefs = {call.value().callId};
        QVERIFY(store.transitionActivity(t));
        QVERIFY(store.pendingExperiences().value().isEmpty());
        auto failure = store.startActivity(intent("failed model"));
        QVERIFY(failure);
        t = transition(failure.value(), ActivityState::Completed);
        t.reason = "MODEL_TIMEOUT";
        QVERIFY(!store.transitionActivity(t));
        t.targetState = ActivityState::Paused;
        QVERIFY(store.transitionActivity(t));
    }
    void emptyCheckRejectsNonemptyCapabilityDataEvenWithEmptyReceipt() {
        QTemporaryDir dir;
        ActivityStore store(profile, dir.filePath("runtime.db"));
        QVERIFY(store.initialize());
        const auto a = store.startActivity(intent("check"));
        QVERIFY(a);
        const auto call = dispatch(store, a.value());
        QVERIFY(call);
        CapabilityOutcome output;
        output.stage = "completed";
        output.executed = true;
        output.data = QJsonObject{{"items", QJsonArray{"actual result"}},
                                  {"empty", false}};
        output.receipt = QJsonObject{{"items", QJsonArray{}}, {"empty", true}};
        auto observation = observed(a.value(), call.value());
        observation.receipt = output.toJson();
        QVERIFY(store.observeAction(observation));
        auto t = transition(a.value(), ActivityState::Completed);
        t.emptyCheck = true;
        t.evidenceRefs = {call.value().callId};
        QVERIFY(!store.transitionActivity(t));
        QCOMPARE(store.find(a.value().activityId).value().state,
                 ActivityState::Running);
    }
    void actionPreparation_rejectsWrongIdentityBudgetAndGeneration() {
        QTemporaryDir dir;
        ActivityStore store(profile, dir.filePath("runtime.db"));
        QVERIFY(store.initialize());
        auto a = store.startActivity(intent());
        QVERIFY(a);
        auto p = action(a.value());
        ++p.generation;
        QVERIFY(!store.prepareAction(p));
        p = action(a.value());
        p.profileId = "other";
        QVERIFY(!store.prepareAction(p));
        auto call = dispatch(store, a.value());
        QVERIFY(call);
        auto o = observed(a.value(), call.value());
        o.profileId = "other";
        QVERIFY(!store.observeAction(o));
        QCOMPARE(store.findAction(call.value().callId).value().status,
                 "dispatched");
    }
    void migration_preservesRepeatedCallsAndBlocksBothLegacyWriteModes() {
        QTemporaryDir dir;
        const auto path = dir.filePath("runtime.db");
        TaskStore legacy;
        QVERIFY(legacy.open(path));
        TaskRecord task;
        task.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        task.profileId = profile;
        task.status = "Executing";
        task.updatedAt =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        task.goal = "old activity";
        task.toolCalls = 3;
        task.activeMs = 420;
        task.personalityVersion = 7;
        task.steps = {QJsonObject{{"id", task.id + ":read:1"},
                                  {"name", "probe"},
                                  {"arguments", QJsonObject{{"query", "same"}}},
                                  {"signature", "same"},
                                  {"read_only", true},
                                  {"status", "succeeded"}},
                      QJsonObject{{"id", task.id + ":read:2"},
                                  {"name", "probe"},
                                  {"arguments", QJsonObject{{"query", "same"}}},
                                  {"signature", "same"},
                                  {"read_only", true},
                                  {"status", "dispatched"}},
                      QJsonObject{{"id", task.id + ":write"},
                                  {"name", "write"},
                                  {"arguments", QJsonObject{{"query", "same"}}},
                                  {"signature", "write:same"},
                                  {"read_only", false},
                                  {"status", "dispatched"}}};
        QVERIFY(legacy.save(task));
        ActivityStore store(profile, path);
        QVERIFY(store.initialize());
        QVERIFY(store.initialize());
        auto row = store.task(task.id);
        QVERIFY(row);
        QCOMPARE(row.value().status, "Paused");
        QCOMPARE(row.value().toolCalls, 3);
        QCOMPARE(row.value().activeMs, 420);
        QCOMPARE(row.value().personalityVersion, 7);
        QCOMPARE(row.value().steps.size(), 3);
        QVERIFY(!legacy.save(task));
        TaskStore reopened;
        QVERIFY(!reopened.open(path));
        QVERIFY(!reopened.isOpen());
        QVERIFY(!reopened.save(task));
        auto a = store.find(task.id);
        QVERIFY(a);
        auto unknown = store.findAction(task.id + ":write");
        QVERIFY(unknown);
        QCOMPARE(unknown.value().status, "unknown");
        QCOMPARE(store.findAction(task.id + ":read:2").value().status,
                 "failed");
        ActionObservation receipt;
        receipt.profileId = profile;
        receipt.activityId = task.id;
        receipt.callId = unknown.value().callId;
        receipt.dispatchGeneration = unknown.value().generation;
        receipt.outcome = ActionOutcome::Succeeded;
        receipt.observedAt = QDateTime::currentDateTimeUtc();
        receipt.receipt = {{"remoteMessageId", "already-written"}};
        QVERIFY(store.observeAction(receipt));
        auto resumed = store.resumeActivity(resume(a.value()));
        QVERIFY(resumed);
        auto p = action(resumed.value(), false);
        p.logicalStepId = "new-model-step";
        p.capabilityRef = {"builtin", "write", "1", "invoke"};
        p.arguments = {{"query", "same"}};
        auto repeated = store.prepareAction(p);
        QVERIFY(repeated);
        QCOMPARE(repeated.value().callId, unknown.value().callId);
    }
    void migration_experienceUsesOriginalSourceAndReadOnlyLegacyOutbox() {
        QTemporaryDir dir;
        const auto path = dir.filePath("runtime.db");
        TaskStore legacy;
        QVERIFY(legacy.open(path));
        TaskRecord task;
        task.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        task.profileId = profile;
        task.goal = "old result";
        task.status = "Completed";
        task.updatedAt =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QVERIFY(legacy.save(task, true));
        ActivityStore store(profile, path);
        QVERIFY(store.initialize());
        auto pending = store.pendingExperiences();
        QVERIFY(pending);
        QCOMPARE(pending.value().size(), 1);
        QCOMPARE(pending.value().first().toObject()["task_id"].toString(),
                 task.id);
        QVERIFY(store.acknowledgeExperience(task.id));
        QVERIFY(store.initialize());
        QVERIFY(store.pendingExperiences().value().isEmpty());
        QCOMPARE(
            sql(path, "SELECT delivered FROM complex_task_experience_outbox")
                .toInt(),
            0);
        QVERIFY(!legacy.acknowledgeExperience(task.id));
    }
    void invalidBudgetAndFailedDispatch_neverInventBudgetOrExecute() {
        QTemporaryDir dir;
        const auto path = dir.filePath("runtime.db");
        ActivityStore store(profile, path);
        QVERIFY(store.initialize());
        auto a = store.startActivity(intent());
        QVERIFY(a);
        auto call = store.prepareAction(action(a.value()));
        QVERIFY(call);
        QVERIFY(
            sql(path,
                "CREATE TRIGGER block_dispatch BEFORE UPDATE ON "
                "agent_action_calls BEGIN SELECT RAISE(ABORT,'blocked'); END")
                .toBool());
        QVERIFY(!store.markActionDispatched(
            {profile, a.value().activityId, call.value().callId,
             a.value().revision, a.value().generation}));
        QCOMPARE(store.find(a.value().activityId).value().budget.toolCalls, 0);
        QCOMPARE(store.findAction(call.value().callId).value().status,
                 "prepared");
        QVERIFY(
            sql(path,
                R"SQL(UPDATE agent_activities SET budget_json='{"limits":{},"used":{}}')SQL")
                .toBool());
        QVERIFY(!store.find(a.value().activityId));
        QVERIFY(!store.resumeActivity(resume(a.value())));
    }
    void terminalOutboxFailure_rollsBackStateAndCallerRevision() {
        QTemporaryDir dir;
        const auto path = dir.filePath("runtime.db");
        TaskStore tasks;
        QVERIFY(tasks.open(path, profile));
        TaskRecord task;
        task.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        task.profileId = profile;
        task.originKind = OriginKind::LocalOwner;
        task.goal = "supplied input";
        task.accepted = true;
        task.evidence = {"user_input"};
        QVERIFY(tasks.save(task));
        const auto revision = task.activityRevision;
        QVERIFY(sql(path,
                    "CREATE TRIGGER block_experience BEFORE INSERT ON "
                    "event_outbox BEGIN SELECT RAISE(ABORT,'blocked'); END")
                    .toBool());
        task.status = "Completed";
        QVERIFY(!tasks.save(task, true));
        QCOMPARE(task.activityRevision, revision);
        QCOMPARE(tasks.find(task.id)->status, "Received");
        QVERIFY(tasks.pendingExperiences().isEmpty());
    }
    void asyncRuntime_drivesSelectedActivityAndKeepsGuiResponsive() {
        QTemporaryDir dir;
        const auto path = dir.filePath("runtime.db");
        ActivityRuntime runtime(profile, path);
        QSignalSpy ready(&runtime, &ActivityRuntime::ready);
        QTRY_COMPARE(ready.size(), 1);
        TaskDependencies dependencies;
        int models = 0;
        dependencies.requestModel =
            [this, &models](const TaskModelRequest &,
                            TaskDependencies::ModelCompletion) {
                QMetaObject::invokeMethod(this, [&models] { ++models; });
            };
        runtime.setDependencies(dependencies);
        int ticks = 0;
        QTimer timer;
        timer.setInterval(2);
        connect(&timer, &QTimer::timeout, this, [&ticks] { ++ticks; });
        timer.start();
        const auto lockName = QUuid::createUuid().toString();
        auto lockDb = QSqlDatabase::addDatabase("QSQLITE", lockName);
        lockDb.setDatabaseName(path);
        QVERIFY(lockDb.open());
        QSqlQuery lock(lockDb);
        QVERIFY(lock.exec("BEGIN IMMEDIATE"));
        bool completed = false;
        runtime.startActivityAsync(intent(), [&](auto result) {
            QVERIFY(result);
            completed = true;
        });
        QTest::qWait(60);
        QVERIFY(ticks > 2);
        QVERIFY(!completed);
        QVERIFY(lock.exec("COMMIT"));
        lock = QSqlQuery();
        lockDb.close();
        lockDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(lockName);
        QTRY_VERIFY(completed);
        QTRY_COMPARE(models, 1);
        QTRY_VERIFY(runtime.isActive());
        QVERIFY(!runtime.taskSnapshot().isEmpty());
        QTRY_VERIFY(ticks > 2);
        runtime.pauseTaskAsync();
        QTRY_VERIFY(!runtime.isActive());
        QCOMPARE(runtime.taskSnapshot()["status"].toString(), "Paused");
    }
    void runtimeUsesLiveCapabilityValidatorOnStartAndResume() {
        QTemporaryDir dir;
        auto write = [&](const QString &name, const QJsonObject &object) {
            QFile file(dir.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            return file.write(QJsonDocument(object).toJson()) > 0;
        };
        QVERIFY(write("plugin.json",
                      {{"formatVersion", "1"},
                       {"id", "test.plugin"},
                       {"name", "Test"},
                       {"version", "1"},
                       {"summary", "Test method"},
                       {"entries", QJsonArray{QJsonObject{
                                       {"kind", "skill"},
                                       {"id", "test.method"},
                                       {"definition", "skill.json"}}}}}));
        QVERIFY(write("skill.json", {{"formatVersion", "1"},
                                     {"id", "test.method"},
                                     {"name", "Method"},
                                     {"version", "1"},
                                     {"summary", "Read supplied text"},
                                     {"instructions", "SKILL.md"},
                                     {"dependencies", QJsonArray{}}}));
        QFile instructions(dir.filePath("SKILL.md"));
        QVERIFY(instructions.open(QIODevice::WriteOnly));
        instructions.write("Read supplied facts.");
        instructions.close();
        // Host settings/database live outside the package digest.
        QTemporaryDir host;
        PluginRuntime plugin(profile, host.filePath("plugins.json"));
        QVERIFY(plugin.registerPlugin({dir.path()}));
        const auto path = host.filePath("runtime.db");
        ActivityRuntime runtime(profile, path);
        QSignalSpy ready(&runtime, &ActivityRuntime::ready);
        QTRY_COMPARE(ready.size(), 1);
        runtime.setCapabilityValidator(plugin.makeCapabilityValidator());
        TaskDependencies dependencies;
        dependencies.requestModel = [](const TaskModelRequest &,
                                       TaskDependencies::ModelCompletion) {};
        runtime.setDependencies(dependencies);
        auto request = intent();
        request.context = ContextSpec::localOwner(profile);
        request.capabilityRefs = {{"test.plugin", "test.method", "1", {}}};
        std::optional<Result<ActivitySnapshot, DomainError>> result;
        runtime.startActivityAsync(
            request, [&](auto value) { result = std::move(value); });
        QTRY_VERIFY(result.has_value());
        QVERIFY2(*result, qPrintable(result->isOk() ? QString()
                                                    : result->error().message));
        const auto id = result->value().activityId;
        QTRY_VERIFY(runtime.isActive());
        runtime.pauseTaskAsync();
        QTRY_VERIFY(!runtime.isActive());
        ActivityStore inspect(profile, path);
        const auto paused = inspect.find(id);
        QVERIFY(paused);
        QVERIFY(plugin.setPluginEnabled(
            {"test.plugin", false, plugin.catalogRevision()}));
        result.reset();
        runtime.resumeActivityAsync(resume(paused.value()), [&](auto value) {
            result = std::move(value);
        });
        QTRY_VERIFY(result.has_value());
        QVERIFY(!*result);
        QCOMPARE(result->error().code, "PLUGIN_DISABLED");
        QVERIFY(plugin.setPluginEnabled(
            {"test.plugin", true, plugin.catalogRevision()}));
        QVERIFY(instructions.open(QIODevice::WriteOnly));
        instructions.write("Changed instructions.");
        instructions.close();
        result.reset();
        runtime.resumeActivityAsync(resume(paused.value()), [&](auto value) {
            result = std::move(value);
        });
        QTRY_VERIFY(result.has_value());
        QVERIFY(!*result);
        QCOMPARE(result->error().code, "STALE_REVISION");
        QVERIFY(plugin.registerPlugin({dir.path()}));
        result.reset();
        runtime.resumeActivityAsync(resume(paused.value()), [&](auto value) {
            result = std::move(value);
        });
        QTRY_VERIFY(result.has_value());
        QVERIFY(*result);
        QTRY_VERIFY(runtime.isActive());
    }
    void typedTransitionPreservesForegroundSnapshotAndElapsedBudget() {
        QTemporaryDir dir;
        const auto path = dir.filePath("runtime.db");
        ActivityRuntime runtime(profile, path);
        QSignalSpy ready(&runtime, &ActivityRuntime::ready);
        QTRY_COMPARE(ready.size(), 1);
        TaskDependencies dependencies;
        dependencies.requestModel = [](const TaskModelRequest &,
                                       TaskDependencies::ModelCompletion) {};
        runtime.setDependencies(dependencies);
        std::optional<ActivitySnapshot> first, second;
        runtime.startActivityAsync(intent("first"), [&](auto result) {
            QVERIFY(result);
            first = result.value();
        });
        QTRY_VERIFY(first.has_value());
        QTRY_VERIFY(runtime.isActive());
        runtime.startActivityAsync(intent("second"), [&](auto result) {
            QVERIFY(result);
            second = result.value();
        });
        QTRY_VERIFY(second.has_value());
        QCOMPARE(second->state, ActivityState::Ready);
        bool completed = false;
        runtime.transitionActivityAsync(
            transition(*second, ActivityState::Paused), [&](auto result) {
                QVERIFY(result);
                completed = true;
            });
        QTRY_VERIFY(completed);
        QCOMPARE(runtime.taskSnapshot()["id"].toString(), first->activityId);
        QTest::qWait(40);
        ActivityStore inspect(profile, path);
        const auto current = inspect.find(first->activityId);
        QVERIFY(current);
        completed = false;
        runtime.transitionActivityAsync(
            transition(current.value(), ActivityState::Paused),
            [&](auto result) {
                QVERIFY(result);
                QVERIFY(result.value().budget.activeMs >= 40);
                completed = true;
            });
        QTRY_VERIFY(completed);
        QTRY_VERIFY(!runtime.isActive());
        QCOMPARE(runtime.taskSnapshot()["status"].toString(), "Paused");
        QVERIFY(runtime.taskSnapshot()["active_ms"].toInteger() >= 40);
        QCOMPARE(inspect.find(first->activityId).value().budget.activeMs,
                 runtime.taskSnapshot()["active_ms"].toInteger());
        const auto paused = inspect.find(first->activityId).value();
        completed = false;
        runtime.resumeActivityAsync(resume(paused), [&](auto result) {
            QVERIFY(result);
            completed = true;
        });
        QTRY_VERIFY(completed);
        QTRY_VERIFY(runtime.isActive());
        auto checkpoint = transition(inspect.find(first->activityId).value(),
                                     ActivityState::Running);
        checkpoint.executionPhase = "checkpoint";
        completed = false;
        runtime.transitionActivityAsync(checkpoint, [&](auto result) {
            QVERIFY(result);
            first = result.value();
            completed = true;
        });
        QTRY_VERIFY(completed);
        QTest::qWait(40);
        auto switchRequest = resume(inspect.find(second->activityId).value());
        switchRequest.expectedForegroundId = first->activityId;
        switchRequest.expectedForegroundRevision = first->revision;
        completed = false;
        runtime.resumeActivityAsync(switchRequest, [&](auto result) {
            QVERIFY(result);
            completed = true;
        });
        QTRY_VERIFY(completed);
        QTRY_COMPARE(runtime.taskSnapshot()["id"].toString(),
                     second->activityId);
        const auto replaced = inspect.find(first->activityId);
        QVERIFY(replaced);
        QCOMPARE(replaced.value().state, ActivityState::Paused);
        QVERIFY(replaced.value().budget.activeMs >=
                paused.budget.activeMs + 40);
    }
    void resumePropagatesContextStorageErrorsInsteadOfUsingOwnerFallback() {
        QTemporaryDir dir;
        const auto path = dir.filePath("runtime.db");
        ActivityStore store(profile, path);
        QVERIFY(store.initialize());
        auto request = intent();
        request.context = ContextSpec::localOwner(profile);
        auto a = store.startActivity(request);
        QVERIFY(a);
        a = store.transitionActivity(
            transition(a.value(), ActivityState::Paused));
        QVERIFY(a);
        QVERIFY(sql(path, "DROP TABLE agent_conversations").toBool());
        const auto result = store.resumeActivity(resume(a.value()));
        QVERIFY(!result);
        QCOMPARE(result.error().code, "STORE_UNAVAILABLE");
        QCOMPARE(store.find(a.value().activityId).value().state,
                 ActivityState::Paused);
    }
    void knownFailedWriteCanRetryButUnknownOrSuccessCannot() {
        QTemporaryDir dir;
        ActivityStore store(profile, dir.filePath("runtime.db"));
        QVERIFY(store.initialize());
        auto a = store.startActivity(intent());
        QVERIFY(a);
        auto first = dispatch(store, a.value(), false);
        QVERIFY(first);
        auto failed = observed(a.value(), first.value(), ActionOutcome::Failed);
        QVERIFY(store.observeAction(failed));
        auto retry = action(a.value(), false);
        retry.logicalStepId = "host-retry-2";
        auto second = store.prepareAction(retry);
        QVERIFY(second);
        QVERIFY(second.value().callId != first.value().callId);
        QVERIFY(store.markActionDispatched(
            {profile, a.value().activityId, second.value().callId,
             a.value().revision, a.value().generation}));
        QCOMPARE(store.find(a.value().activityId).value().budget.toolCalls, 2);
        QVERIFY(store.observeAction(
            observed(a.value(), second.value(), ActionOutcome::Unknown)));
        retry.logicalStepId = "host-retry-3";
        auto same = store.prepareAction(retry);
        QVERIFY(same);
        QCOMPARE(same.value().callId, second.value().callId);
    }
    void completionRejectsEveryUnsettledActionInTypedAndCompatibilityPaths() {
        for (const auto &status :
             QStringList{"prepared", "dispatched", "accepted", "unknown"}) {
            QTemporaryDir dir;
            ActivityStore store(profile, dir.filePath("runtime.db"));
            QVERIFY(store.initialize());
            const auto started = store.startActivity(intent());
            QVERIFY(started);
            const auto a = started.value();
            const auto first = dispatch(store, a);
            QVERIFY(first);
            QVERIFY(store.observeAction(observed(a, first.value())));
            auto next = action(a, false);
            next.logicalStepId = "second-action";
            next.arguments = {{"query", "unsettled operation"}};
            auto second = store.prepareAction(next);
            QVERIFY(second);
            if (status != "prepared") {
                second = store.markActionDispatched({profile, a.activityId,
                                                     second.value().callId,
                                                     a.revision, a.generation});
                QVERIFY(second);
            }
            if (status == "accepted" || status == "unknown") {
                auto observation =
                    observed(a, second.value(),
                             status == "accepted" ? ActionOutcome::Accepted
                                                  : ActionOutcome::Unknown);
                observation.receipt = {
                    {"tracking", QJsonObject{{"sourceId", "jobs"},
                                             {"remoteJobId", "job"}}}};
                QVERIFY(store.observeAction(observation));
            }
            auto completion = transition(a, ActivityState::Completed);
            completion.evidenceRefs = {first.value().callId};
            QVERIFY2(!store.transitionActivity(completion), qPrintable(status));
            auto task = store.task(a.activityId);
            QVERIFY(task);
            task.value().status = "Completed";
            task.value().evidence = QJsonArray{first.value().callId};
            QVERIFY2(!store.saveTask(task.value(), true), qPrintable(status));
            QCOMPARE(store.find(a.activityId).value().state,
                     ActivityState::Running);
            QVERIFY(store.pendingExperiences().value().isEmpty());
        }
    }
    void pendingReplyEvents_filtersHistoricalFloodBeforeLimit() {
      QTemporaryDir dir;
      const auto path = dir.filePath("runtime.db");
      ActivityStore store(profile, path);
      QVERIFY(store.initialize());
      ConversationStore messages(profile, path);
      QVERIFY(messages.initialize());
      HostConversationBinding binding;
      binding.channel = "chat";
      binding.connectionKey = "conn";
      binding.remoteConversationId = "room";
      QVERIFY(messages.registerBinding(binding));
      IncomingMessage message;
      message.channel = "chat";
      message.connectionKey = "conn";
      message.remoteConversationId = "room";
      message.remoteMessageId = "first";
      message.content = "First message";
      message.speakerIdentity = {"chat", "conn", "friend", "Friend"};
      message.occurredAt = QDateTime::currentDateTimeUtc();
      auto first = messages.acceptMessage(message);
      QVERIFY(first);
      auto i = intent("chat");
      i.origin = "self";
      i.context = first.value().context;
      auto activity = store.startActivity(i);
      QVERIFY(activity);
      auto call = dispatch(store, activity.value(), false);
      QVERIFY(call);
      QVERIFY(store.observeAction(observed(activity.value(), call.value())));
      const auto anchor = sendMessage(messages, first.value());
      QVERIFY(!anchor.isEmpty());
      activity = store.find(activity.value().activityId);
      auto waiting =
          transition(activity.value(), ActivityState::WaitingExternal);
      waiting.waitingCondition = {
          {"kind", "message"},
          {"conversationId", first.value().conversationId},
          {"afterMessageId", anchor},
          {"callId", call.value().callId},
          {"resumeRule", "event"}};
      QVERIFY(store.transitionActivity(waiting));
      message.remoteMessageId = "matching-reply";
      message.content = "The actual reply";
      auto reply = messages.acceptMessage(message);
      QVERIFY(reply);
      message.historicalImport = true;
      for (int index = 0; index < 260; ++index) {
        message.remoteMessageId = "history-" + QString::number(index);
        message.content = "Imported history";
        QVERIFY(messages.acceptMessage(message));
      }
      auto candidates = store.pendingReplyEvents();
      QVERIFY(candidates);
      QCOMPARE(candidates.value(), QStringList{reply.value().originId});
      QVERIFY(store.routeObservedReply(reply.value().originId).value());
    }
    void
    acceptedTrackedActionWaitsAndResumesOnlyAfterVerifiedCompletion_data() {
      QTest::addColumn<QString>("origin");
      for (const auto &origin : {"user", "self", "event"})
        QTest::newRow(origin) << QString(origin);
    }
    void acceptedTrackedActionWaitsAndResumesOnlyAfterVerifiedCompletion() {
      QFETCH(QString, origin);
      QTemporaryDir dir;
      const auto path = dir.filePath("runtime.db");
      ActivityStore store(profile, path);
      QVERIFY(store.initialize());
      auto i = intent();
      i.origin = origin;
      auto a = store.startActivity(i);
      QVERIFY(a);
      auto call = dispatch(store, a.value(), false);
      QVERIFY(call);
      auto accepted =
          observed(a.value(), call.value(), ActionOutcome::Accepted);
      accepted.receipt = {{"tracking", QJsonObject{{"sourceId", "job-service"},
                                                   {"remoteJobId", "job-1"}}}};
      QVERIFY(store.observeAction(accepted));
      auto t = transition(a.value(), ActivityState::WaitingExternal);
      t.waitingCondition = {{"kind", "event"},
                            {"callId", call.value().callId},
                            {"sourceId", "job-service"},
                            {"remoteJobId", "job-1"},
                            {"resumeRule", "event"}};
      auto waiting = store.transitionActivity(t);
      QVERIFY(waiting);
      auto completed = observed(a.value(), call.value());
      completed.receipt = {{"sourceId", "job-service"},
                           {"remoteJobId", "job-1"},
                           {"done", true}};
      auto incorrect = completed;
      incorrect.receipt["sourceId"] = "other-service";
      QVERIFY(!store.observeAction(incorrect));
      incorrect = completed;
      incorrect.receipt["remoteJobId"] = "other-job";
      QVERIFY(!store.observeAction(incorrect));
      QCOMPARE(store.findAction(call.value().callId).value().status,
               "accepted");
      QVERIFY(store.observeAction(completed));
      QVERIFY(store.observeAction(completed));
      ConversationStore events(profile, path);
      QVERIFY(events.recoverOutbox());
      QCOMPARE(
          sql(path,
              "SELECT COUNT(*) FROM event_log WHERE type='ActionCompleted'")
              .toInt(),
          1);
      const auto trigger =
          sql(path,
              "SELECT event_id FROM event_log WHERE type='ActionCompleted'")
              .toString();
      QVERIFY(!trigger.isEmpty());
      auto pending = store.pendingReplyEvents();
      QVERIFY(pending);
      if (origin != "user") {
        QVERIFY(pending.value().contains(trigger));
        auto routed = store.routeObservedReply(trigger);
        QVERIFY(routed && routed.value());
        QVERIFY(!store.routeObservedReply(trigger).value());
        auto ready = store.find(a.value().activityId);
        QVERIFY(ready);
        QCOMPARE(ready.value().state, ActivityState::Ready);
        QCOMPARE(ready.value().waitingCondition["resumeEventId"].toString(),
                 trigger);
        QCOMPARE(ready.value().budget.toJson(),
                 waiting.value().budget.toJson());
        return;
      }
      QVERIFY(pending.value().isEmpty());
      QVERIFY(!store.routeObservedReply(trigger).value());
      QCOMPARE(store.find(a.value().activityId).value().state,
               ActivityState::WaitingExternal);
      auto r = resume(waiting.value());
      r.resumeActor = ResumeActor::Scheduler;
      r.triggerRef = trigger;
      QVERIFY(!store.resumeActivity(r));
      r.resumeActor = ResumeActor::Event;
      auto resumed = store.resumeActivity(r);
      QVERIFY(resumed);
    }
};
QTEST_GUILESS_MAIN(TestActivityRuntime)
#include "test_activity_runtime.moc"
