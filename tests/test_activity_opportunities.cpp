#include "ai/activity/activity_runtime.h"
#include "ai/activity/activity_store.h"
#include "ai/activity/opportunity_store.h"
#include "ai/event/runtime_unit_of_work.h"
#include "ai/model/model_router.h"
#include "ai/scheduler/opportunity_types.h"
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <limits>

namespace {
const QString profile = "11111111-1111-4111-8111-111111111111";
OpportunitySignal signal(qint64 time = 0) {
  OpportunitySignal s;
  s.clock = {
      QDateTime::fromString("2026-10-16T00:00:00.000Z", Qt::ISODateWithMs)
          .addMSecs(time),
      time, "run-1"};
  s.readiness.running = true;
  s.readiness.autonomyEnabled = true;
  s.readiness.hostRevision = 1;
  return s;
}
OpportunityFinish finish(const OpportunityTicket &t,
                         const OpportunityClock &clock) {
  OpportunityFinish f;
  f.ticketId = t.ticketId;
  f.hostRevision = t.hostRevision;
  f.inputWatermark = t.inputWatermark;
  f.clock = clock;
  return f;
}
struct Fixture {
  QTemporaryDir dir;
  QString path = dir.filePath("runtime.db");
  ActivityStore activities{profile, path};
  OpportunityStore store{profile, path};
  AutonomyConfig config;
  Fixture() {
    config.enabled = true;
    if (!activities.initialize() || !store.configure(config, signal().clock))
      qFatal("fixture setup");
  }
  void execute(const QString &statement) {
    const auto name = QUuid::createUuid().toString();
    {
      auto db = QSqlDatabase::addDatabase("QSQLITE", name);
      db.setDatabaseName(path);
      QVERIFY(db.open());
      QSqlQuery q(db);
      QVERIFY2(q.exec(statement), qPrintable(q.lastError().text()));
    }
    QSqlDatabase::removeDatabase(name);
  }
};
class BudgetClient : public ModelCompletionClient {
public:
  int calls = 0;
  void completeOnce(const ModelRouteConfig &, const QList<ChatMessage> &,
                    const QJsonArray &, LlmCompletionHandler done,
                    const QString &) override {
    ++calls;
    LlmResponse r;
    r.content = "{\"ok\":true}";
    r.usage.totalTokens = 20;
    if (calls == 1)
      done(false, {}, "network timeout");
    else
      done(true, r, {});
  }
};
class LedgerClient : public ModelCompletionClient {
public:
  bool deferred = false;
  int calls = 0;
  LlmCompletionHandler pending;
  void completeOnce(const ModelRouteConfig &, const QList<ChatMessage> &,
                    const QJsonArray &, LlmCompletionHandler done,
                    const QString &) override {
    ++calls;
    if (deferred) {
      pending = std::move(done);
      return;
    }
    LlmResponse response;
    response.content = calls == 1 ? "invalid" : "{\"ok\":true}";
    response.usage.totalTokens = 20;
    done(true, response, {});
  }
};
ModelRoleConfig ledgerRole() {
  ModelRoleConfig role;
  role.role = ModelRole::TaskExecution;
  ModelRouteConfig route;
  route.routeId = "test";
  route.enabled = route.llm.enabled = true;
  route.llm.provider = "openai-compatible";
  route.llm.baseUrl = "https://test.invalid";
  route.llm.apiKey = "test";
  route.llm.model = "test";
  role.routes = {route};
  return role;
}
} // namespace

class TestActivityOpportunities : public QObject {
  Q_OBJECT
private slots:
  void modelBudget_duplicateProviderReceiptsSettleWithoutCompletingTwice() {
    LedgerClient client;
    client.deferred = true;
    ModelRoleRegistry registry({ledgerRole()});
    ModelRouter router(&registry, &client);
    ModelRequest request;
    request.role = ModelRole::TaskExecution;
    auto hooks = std::make_shared<ModelAttemptHooks>();
    int reservations = 0, completions = 0;
    QList<LlmUsage> receipts;
    QList<ModelAttemptHooks::Completion> settlements;
    hooks->reserve = [&](const ModelAttemptContext &,
                         ModelAttemptHooks::Completion done) {
      ++reservations;
      done(Result<void, DomainError>::success());
    };
    hooks->settle = [&](const QString &, const LlmUsage &usage,
                        ModelAttemptHooks::Completion done) {
      receipts.append(usage);
      settlements.append(std::move(done));
    };
    request.attemptHooks = hooks;
    router.completeAsync(request, [&](auto r) {
      QVERIFY(r);
      ++completions;
    });
    QVERIFY(bool(client.pending));
    LlmResponse response;
    response.content = "done";
    client.pending(true, response, {});
    response.usage.totalTokens = 42;
    client.pending(true, response, {});
    QCOMPARE(client.calls, 1);
    QCOMPARE(reservations, 1);
    QCOMPARE(settlements.size(), 2);
    QCOMPARE(completions, 0);
    QCOMPARE(receipts.last().totalTokens, 42);
    settlements.last()(Result<void, DomainError>::success());
    QCOMPARE(completions, 0);
    settlements.first()(Result<void, DomainError>::success());
    QCOMPARE(completions, 1);
  }

  void sharedRuntimeTransaction_serializesWritesBeforeReadingCasVersion() {
    Fixture f;
    auto started = SqliteRuntimeUnitOfWork::begin(f.path);
    QVERIFY(started);
    auto work = started.takeValue();
    QSqlQuery current(QSqlDatabase::database(work->connectionName()));
    QVERIFY(current.exec("SELECT revision FROM agent_runtime_state"));
    QVERIFY(current.next());
    const auto revision = current.value(0).toLongLong();
    current.finish();
    const QString name = QUuid::createUuid().toString();
    bool competingWrite = false;
    {
      auto competitor = QSqlDatabase::addDatabase("QSQLITE", name);
      competitor.setDatabaseName(f.path);
      QVERIFY(competitor.open());
      QSqlQuery query(competitor);
      QVERIFY(query.exec("PRAGMA busy_timeout=0"));
      competingWrite =
          query.exec("UPDATE agent_runtime_state SET revision=revision+1");
    }
    QSqlDatabase::removeDatabase(name);
    QSqlQuery update(QSqlDatabase::database(work->connectionName()));
    update.prepare(
        "UPDATE agent_runtime_state SET revision=revision+1 WHERE revision=?");
    update.addBindValue(revision);
    QVERIFY2(update.exec(), qPrintable(update.lastError().text()));
    QCOMPARE(update.numRowsAffected(), 1);
    QVERIFY(!competingWrite);
    QVERIFY(work->commit());
    f.execute("UPDATE agent_runtime_state SET revision=revision+1");
  }

  void cancelledOrRetiredRouterClosesReservedAttemptWithoutProviderSend() {
    QTemporaryDir dir;
    const auto path = dir.filePath("runtime.db");
    ActivityRuntime runtime(profile, path);
    QSignalSpy ready(&runtime, &ActivityRuntime::ready);
    QTRY_COMPARE(ready.size(), 1);
    AutonomyConfig config;
    config.enabled = true;
    bool configured = false;
    runtime.configureOpportunitiesAsync(config, [&](auto r) {
      QVERIFY(r);
      configured = true;
    });
    QTRY_VERIFY(configured);
    OpportunityTicket ticket;
    runtime.setOpportunityConsumer([&](auto t) { ticket = t; });
    auto s = signal();
    s.clock = OpportunityStore::systemClock();
    runtime.requestOpportunityAsync(s);
    QTRY_VERIFY(!ticket.ticketId.isEmpty());
    auto actual = runtime.modelBudgetHooks({}, ticket.ticketId);
    auto hooks = std::make_shared<ModelAttemptHooks>(*actual);
    std::function<void()> release;
    hooks->reserve = [&](const ModelAttemptContext &context,
                         ModelAttemptHooks::Completion done) {
      actual->reserve(
          context, [&, done = std::move(done)](auto result) mutable {
            QMetaObject::invokeMethod(
                this, [&, done = std::move(done), result]() mutable {
                  release = [done = std::move(done), result]() mutable {
                    done(result);
                  };
                });
          });
    };
    LedgerClient client;
    ModelRoleRegistry registry({ledgerRole()});
    ModelRequest request;
    request.role = ModelRole::TaskExecution;
    request.attemptHooks = hooks;
    OpportunityStore store(profile, path);
    for (bool retire : {false, true}) {
      auto router = std::make_unique<ModelRouter>(&registry, &client);
      bool finished = false;
      auto handle = router->completeStreamAsync(request, {}, [&](auto r) {
        QVERIFY(!r);
        finished = true;
      });
      QTRY_VERIFY(bool(release));
      if (retire)
        router.reset();
      else
        handle->cancel();
      auto completeReservation = std::exchange(release, {});
      completeReservation();
      QTRY_VERIFY([&] {
        const auto attempts = store.snapshot()
                                  .value()["hourlyBudget"]
                                  .toObject()["attempts"]
                                  .toObject();
        for (const auto &value : attempts)
          if (!value.toObject()["settled"].toBool())
            return false;
        return !attempts.isEmpty();
      }());
      QCOMPARE(client.calls, 0);
      QCOMPARE(finished, !retire);
    }
  }
  void
  bindHostEpochSettlesOrphanReservationBeforeConfigureAndRejectsOldControls() {
    Fixture f;
    QVERIFY(f.store.bindHostEpoch("old", signal().clock));
    auto s = signal();
    s.readiness.hostEpoch = "old";
    auto t = f.store.considerOpportunity(s);
    QVERIFY(t);
    ModelBudgetRequest r;
    r.profileId = profile;
    r.attemptId = "orphan";
    r.ticketId = t.value().ticketId;
    r.purpose = "selection";
    r.maxOutputTokens = 10;
    r.clock = s.clock;
    QVERIFY(f.store.reserveModelAttempt(r));
    OpportunityStore replacement(profile, f.path);
    QVERIFY(replacement.bindHostEpoch("new", s.clock));
    QVERIFY(replacement.snapshot()
                .value()["hourlyBudget"]
                .toObject()["attempts"]
                .toObject()["orphan"]
                .toObject()["settled"]
                .toBool());
    QVERIFY(!f.store.configure(f.config, s.clock));
    QVERIFY(!f.store.setPaused(true, s.clock));
    QVERIFY(replacement.configure(f.config, s.clock));
  }
  void
  runtimeModelHooks_schemaRepairChargesRealLedgerAndLateReceiptSurvivesRetirement() {
    QTemporaryDir dir;
    const QString path = dir.filePath("runtime.db");
    auto runtime = std::make_unique<ActivityRuntime>(profile, path);
    QSignalSpy ready(runtime.get(), &ActivityRuntime::ready);
    QTRY_COMPARE(ready.size(), 1);
    AutonomyConfig config;
    config.enabled = true;
    bool configured = false;
    runtime->configureOpportunitiesAsync(config, [&](auto r) {
      QVERIFY(r);
      configured = true;
    });
    QTRY_VERIFY(configured);
    OpportunityTicket ticket;
    runtime->setOpportunityConsumer([&](auto t) { ticket = t; });
    auto s = signal();
    s.clock = OpportunityStore::systemClock();
    runtime->requestOpportunityAsync(s);
    QTRY_VERIFY(!ticket.ticketId.isEmpty());
    LedgerClient client;
    ModelRoleRegistry registry({ledgerRole()});
    auto router = std::make_unique<ModelRouter>(&registry, &client);
    ModelRequest request;
    request.role = ModelRole::TaskExecution;
    request.responseSchema = {
        {"type", "object"},
        {"required", QJsonArray{"ok"}},
        {"properties", QJsonObject{{"ok", QJsonObject{{"type", "boolean"}}}}}};
    request.attemptHooks = runtime->modelBudgetHooks({}, ticket.ticketId);
    bool completed = false;
    router->completeAsync(request, [&](auto r) {
      QVERIFY(r);
      completed = true;
    });
    QTRY_VERIFY(completed);
    QCOMPARE(client.calls, 2);
    OpportunityStore store(profile, path);
    auto state = store.snapshot();
    QVERIFY(state);
    auto attempts =
        state.value()["hourlyBudget"].toObject()["attempts"].toObject();
    QCOMPARE(attempts.size(), 2);
    for (const auto &attempt : attempts) {
      QVERIFY(attempt.toObject()["usageKnown"].toBool());
      QCOMPARE(attempt.toObject()["tokens"].toInteger(), 20);
    }
    // A new runtime/ticket, after the configured minimum interval, provides a
    // separate provider attempt whose actual callback outlives both facades.
    Fixture late;
    late.config.minDecisionIntervalMs = 1;
    QVERIFY(late.store.configure(late.config, signal().clock));
    runtime.reset();
    runtime = std::make_unique<ActivityRuntime>(profile, late.path);
    QSignalSpy lateReady(runtime.get(), &ActivityRuntime::ready);
    QTRY_COMPARE(lateReady.size(), 1);
    configured = false;
    runtime->configureOpportunitiesAsync(late.config, [&](auto r) {
      QVERIFY(r);
      configured = true;
    });
    QTRY_VERIFY(configured);
    ticket = {};
    runtime->setOpportunityConsumer([&](auto t) { ticket = t; });
    s.clock = OpportunityStore::systemClock();
    runtime->requestOpportunityAsync(s);
    QTRY_VERIFY(!ticket.ticketId.isEmpty());
    client.deferred = true;
    request.responseSchema = {};
    request.attemptHooks = runtime->modelBudgetHooks({}, ticket.ticketId);
    completed = false;
    router->completeAsync(request, [&](auto) { completed = true; });
    QTRY_VERIFY(bool(client.pending));
    runtime.reset();
    router.reset();
    LlmResponse actual;
    actual.content = "done";
    actual.usage.totalTokens = 42;
    client.pending(true, actual, {});
    QTRY_VERIFY(late.store.snapshot()
                    .value()["hourlyBudget"]
                    .toObject()["attempts"]
                    .toObject()
                    .begin()
                    .value()
                    .toObject()["usageKnown"]
                    .toBool());
    QCOMPARE(late.store.snapshot()
                 .value()["hourlyBudget"]
                 .toObject()["attempts"]
                 .toObject()
                 .begin()
                 .value()
                 .toObject()["tokens"]
                 .toInteger(),
             42);
    QVERIFY(!completed);
  }
  void realUsageOverflowBlocksNextAttemptAndArchivedAttemptCannotBeReused() {
    Fixture f;
    auto t = f.store.considerOpportunity(signal());
    QVERIFY(t);
    ModelBudgetRequest r;
    r.profileId = profile;
    r.attemptId = "huge";
    r.ticketId = t.value().ticketId;
    r.purpose = "selection";
    r.maxOutputTokens = 10;
    r.clock = signal().clock;
    QVERIFY(f.store.reserveModelAttempt(r));
    ModelBudgetSettlement actual;
    actual.profileId = profile;
    actual.attemptId = r.attemptId;
    actual.clock = r.clock;
    actual.usage.totalTokens = std::numeric_limits<qint64>::max();
    QVERIFY(f.store.settleModelAttempt(actual));
    r.attemptId = "next";
    auto denied = f.store.reserveModelAttempt(r);
    QVERIFY(!denied);
    QCOMPARE(denied.error().code, QString("BUDGET_EXHAUSTED"));
    QVERIFY(f.store.considerOpportunity(signal(3600001)));
    QVERIFY(f.store.snapshot()
                .value()["hourlyBudget"]
                .toObject()["attempts"]
                .toObject()
                .isEmpty());
    r.attemptId = "huge";
    r.clock = signal(3600001).clock;
    auto reused = f.store.reserveModelAttempt(r);
    QVERIFY(!reused);
    QCOMPARE(reused.error().code, QString("STALE_REVISION"));
    QVERIFY(f.store.settleModelAttempt(actual));
  }
  void activeBudgetExhaustedBlocksNewAutonomousStartResumeAndAction() {
    Fixture f;
    f.config.maxActiveMsPerHour = 100;
    QVERIFY(f.store.configure(f.config, signal().clock));
    ActivityIntent intent;
    intent.goal = "read";
    intent.origin = "self";
    intent.context.profileId = profile;
    auto a = f.activities.startActivity(intent);
    QVERIFY(a);
    ActivityTransition t;
    t.profileId = profile;
    t.activityId = a.value().activityId;
    t.expectedRevision = a.value().revision;
    t.targetState = ActivityState::Running;
    t.activeMs = 100;
    auto running = f.activities.transitionActivity(t);
    QVERIFY(running);
    ActionPreparation p;
    p.profileId = profile;
    p.activityId = t.activityId;
    p.expectedRevision = running.value().revision;
    p.generation = running.value().generation;
    p.logicalStepId = "one";
    p.capabilityRef = {"builtin", "read", "1", "invoke"};
    p.readOnly = true;
    QVERIFY(!f.activities.prepareAction(p));
    t.targetState = ActivityState::Paused;
    t.expectedRevision = running.value().revision;
    t.reason = "user";
    auto paused = f.activities.transitionActivity(t);
    QVERIFY(paused);
    ResumeActivityRequest resume;
    resume.profileId = profile;
    resume.activityId = t.activityId;
    resume.expectedRevision = paused.value().revision;
    resume.resumeActor = ResumeActor::User;
    QVERIFY(!f.activities.resumeActivity(resume));
    QVERIFY(!f.activities.startActivity(intent));
    intent.origin = "user";
    QVERIFY(f.activities.startActivity(intent));
  }
  void restartHostEpochRejectsRetiredReadinessAndPreservesObservedWatermarks() {
    Fixture f;
    QVERIFY(f.store.bindHostEpoch("first-host", signal().clock));
    auto s = signal();
    s.readiness.hostEpoch = "first-host";
    s.readiness.hostRevision = 200;
    s.inputWatermark = {{"chat", "committed-1"}};
    auto t = f.store.considerOpportunity(s);
    QVERIFY(t);
    QVERIFY(f.store.finishOpportunity(finish(t.value(), s.clock)));
    QVERIFY(f.store.bindHostEpoch("second-host", signal(60000).clock));
    QVERIFY(!f.store.publishReadiness(s));
    s = signal(60000);
    s.readiness.hostEpoch = "second-host";
    QVERIFY(f.store.publishReadiness(s));
    QVERIFY(!f.store.considerOpportunity(s)); // Old observed input is not new.
    s.inputWatermark = {{"chat", "committed-2"}};
    s.readiness.hostRevision = 2;
    QVERIFY(f.store.considerOpportunity(s));
  }
  void expiredReservationClearsEvenWhenBusyAndConfigPauseRevokesTicket() {
    Fixture f;
    auto t = f.store.considerOpportunity(signal());
    QVERIFY(t);
    auto busy = signal(120001);
    busy.readiness.readyObligations = {"due"};
    QVERIFY(!f.store.considerOpportunity(busy));
    QVERIFY(f.store.snapshot()
                .value()["selectionReservation"]
                .toObject()
                .isEmpty());
    auto t2 = f.store.considerOpportunity(signal(120001));
    QVERIFY(t2);
    f.config.paused = true;
    QVERIFY(f.store.configure(f.config, signal(120001).clock));
    QVERIFY(f.store.snapshot()
                .value()["selectionReservation"]
                .toObject()
                .isEmpty());
    f.config.paused = false;
    QVERIFY(f.store.configure(f.config, signal(180001).clock));
    QVERIFY(f.store.considerOpportunity(signal(180001)));
  }
  void
  clockJumpAndControlJsonMerge_preserveWindowReservationsAndSourceCursors() {
    Fixture f;
    f.execute("UPDATE agent_runtime_state SET "
              "control_json=json_set(control_json,'$.sourceCursors',json('{"
              "\"qq\":\"cursor-7\"}'))");
    f.config.maxSelectionCallsPerHour = 1;
    QVERIFY(f.store.configure(f.config, signal().clock));
    auto t = f.store.considerOpportunity(signal());
    QVERIFY(t);
    ModelBudgetRequest r;
    r.profileId = profile;
    r.attemptId = "clock-attempt";
    r.ticketId = t.value().ticketId;
    r.purpose = "selection";
    r.maxOutputTokens = 10;
    r.clock = signal().clock;
    QVERIFY(f.store.reserveModelAttempt(r));
    QVERIFY(f.store.settleModelAttempt({profile, r.attemptId, {}, r.clock}));
    auto ended = finish(t.value(), r.clock);
    ended.outcome = "cancel";
    QVERIFY(f.store.finishOpportunity(ended));
    auto forward = signal(61000);
    forward.clock.utc = forward.clock.utc.addYears(1);
    auto no = f.store.considerOpportunity(forward);
    QVERIFY(!no);
    QCOMPARE(no.error().code, QString("BUDGET_EXHAUSTED"));
    QVERIFY(f.store.configure(f.config, forward.clock));
    auto backwards = signal(3600001);
    backwards.clock.utc = backwards.clock.utc.addYears(-1);
    QVERIFY(!f.store.considerOpportunity(backwards));
    auto restarted = signal();
    restarted.clock.sessionId = "new-process";
    QVERIFY(f.store.configure(f.config, restarted.clock));
    QVERIFY(!f.store.considerOpportunity(restarted));
    QCOMPARE(
        f.store.snapshot().value()["sourceCursors"].toObject()["qq"].toString(),
        QString("cursor-7"));
  }
  void corruptLedgerFailsClosedAndSharedBudgetBlocksAutonomousActivity() {
    Fixture f;
    f.config.maxTokensPerHour = 2000;
    QVERIFY(f.store.configure(f.config, signal().clock));
    auto t = f.store.considerOpportunity(signal());
    QVERIFY(t);
    ModelBudgetRequest r;
    r.profileId = profile;
    r.attemptId = "selection";
    r.ticketId = t.value().ticketId;
    r.purpose = "selection";
    r.maxOutputTokens = 100;
    r.clock = signal().clock;
    QVERIFY(f.store.reserveModelAttempt(r));
    auto ended = finish(t.value(), r.clock);
    ended.outcome = "cancel";
    QVERIFY(f.store.finishOpportunity(ended));
    ActivityIntent intent;
    intent.goal = "read";
    intent.origin = "self";
    intent.context.profileId = profile;
    auto a = f.activities.startActivity(intent);
    QVERIFY(a);
    r.attemptId = "activity";
    r.ticketId.clear();
    r.activityId = a.value().activityId;
    r.purpose = "activity";
    auto budget = f.store.reserveModelAttempt(r);
    QVERIFY(!budget);
    QCOMPARE(budget.error().code, QString("BUDGET_EXHAUSTED"));
    f.execute("UPDATE agent_runtime_state SET "
              "control_json=json_set(control_json,'$.hourlyBudget.attempts', "
              "'broken')");
    auto corrupted = f.store.reserveModelAttempt(r);
    QVERIFY(!corrupted);
    QCOMPARE(corrupted.error().code, QString("STORE_UNAVAILABLE"));
  }
  void activeRuntimePausesAtHourlyLimit() {
    QTemporaryDir dir;
    ActivityRuntime runtime(profile, dir.filePath("runtime.db"));
    QSignalSpy ready(&runtime, &ActivityRuntime::ready);
    QTRY_COMPARE(ready.size(), 1);
    AutonomyConfig config;
    config.enabled = true;
    config.maxActiveMsPerHour = 150;
    bool configured = false;
    runtime.configureOpportunitiesAsync(config, [&](auto r) {
      QVERIFY(r);
      configured = true;
    });
    QTRY_VERIFY(configured);
    TaskDependencies dependencies;
    dependencies.requestModel = [](const TaskModelRequest &,
                                   TaskDependencies::ModelCompletion) {};
    runtime.setDependencies(dependencies);
    ActivityIntent intent;
    intent.goal = "read slowly";
    intent.origin = "self";
    intent.context.profileId = profile;
    bool started = false;
    runtime.startActivityAsync(intent, [&](auto r) {
      QVERIFY(r);
      started = true;
    });
    QTRY_VERIFY(started);
    QTRY_COMPARE_WITH_TIMEOUT(runtime.taskSnapshot()["status"].toString(),
                              QString("Paused"), 3000);
    QVERIFY(!runtime.isActive());
    QVERIFY(runtime.taskSnapshot()["active_ms"].toInteger() >= 150);
  }
  void
  considerOpportunity_messageBurst_coalescesOnlySameConversationWithinBound() {
    QList<CoalescedMessage> messages{
        {"a", "a1", 0}, {"b", "b1", 100}, {"a", "a2", 900}};
    AutonomyConfig config;
    auto batch = nextMessageBatch(messages, config);
    QCOMPARE(batch.contextId, QString("b"));
    QCOMPARE(batch.dueAtMonotonicMs, 1100);
    QCOMPARE(batch.messageIds, QStringList{"b1"});
    messages.removeAt(1);
    messages.append({"a", "a3", 1900});
    messages.append({"a", "a4", 2800});
    messages.append({"a", "a5", 3500});
    batch = nextMessageBatch(messages, config);
    QCOMPARE(batch.dueAtMonotonicMs, 3000);
    QCOMPARE(batch.messageIds, (QStringList{"a1", "a2", "a3", "a4"}));
  }
  void runtime_noConsumerLocalOnly_thenControlledConsumerReceivesOneTicket() {
    QTemporaryDir dir;
    ActivityRuntime runtime(profile, dir.filePath("runtime.db"));
    QSignalSpy ready(&runtime, &ActivityRuntime::ready);
    QTRY_COMPARE(ready.size(), 1);
    AutonomyConfig config;
    config.enabled = true;
    bool configured = false;
    runtime.configureOpportunitiesAsync(config, [&](auto r) {
      QVERIFY(r);
      configured = true;
    });
    QTRY_VERIFY(configured);
    auto s = signal();
    s.clock = OpportunityStore::systemClock();
    runtime.publishReadinessAsync(s);
    runtime.requestOpportunityAsync(s);
    QTest::qWait(50);
    OpportunityStore store(profile, dir.filePath("runtime.db"));
    auto state = store.snapshot();
    QVERIFY(state);
    QVERIFY(state.value()["selectionReservation"].toObject().isEmpty());
    int tickets = 0;
    runtime.setOpportunityConsumer([&](OpportunityTicket t) {
      QVERIFY(!t.ticketId.isEmpty());
      ++tickets;
    });
    runtime.requestOpportunityAsync(s);
    QTRY_COMPARE(tickets, 1);
    runtime.requestOpportunityAsync(s);
    QTest::qWait(30);
    QCOMPARE(tickets, 1);
  }
  void activeBudget_runningCountsWaitingDoesNot_preservesRestartBase() {
    Fixture f;
    f.config.maxActiveMsPerHour = 1000;
    QVERIFY(f.store.configure(f.config, signal().clock));
    ActivityIntent intent;
    intent.goal = "autonomous read";
    intent.origin = "self";
    intent.context.profileId = profile;
    auto a = f.activities.startActivity(intent);
    QVERIFY(a);
    ActivityTransition t;
    t.profileId = profile;
    t.activityId = a.value().activityId;
    t.expectedRevision = a.value().revision;
    t.targetState = ActivityState::Paused;
    t.reason = "checkpoint";
    t.activeMs = 700;
    QVERIFY(f.activities.transitionActivity(t));
    auto state = f.store.snapshot();
    QVERIFY(state);
    QCOMPARE(state.value()["hourlyBudget"]
                 .toObject()["activeSamples"]
                 .toObject()[t.activityId]
                 .toInteger(),
             700);
    auto current = f.activities.find(t.activityId);
    QVERIFY(current);
    t.expectedRevision = current.value().revision;
    t.activeMs = 700;
    QVERIFY(f.activities.transitionActivity(t));
    auto remaining = f.activities.remainingAutonomousActiveMs();
    QVERIFY(remaining);
    QCOMPARE(remaining.value(), 300);
  }
  void modelBudget_eachProviderAttemptReservesAndSettlesBeforeFallback() {
    BudgetClient client;
    ModelRoleConfig config;
    config.role = ModelRole::TaskExecution;
    for (const auto &id : QStringList{"first", "fallback"}) {
      ModelRouteConfig r;
      r.routeId = id;
      r.enabled = true;
      r.llm.enabled = true;
      r.llm.baseUrl = "https://test.invalid";
      r.llm.apiKey = "test";
      r.llm.model = "test";
      r.llm.provider = "openai-compatible";
      config.routes.append(r);
    }
    ModelRoleRegistry registry({config});
    ModelRouter router(&registry, &client);
    ModelRequest request;
    request.role = config.role;
    auto hooks = std::make_shared<ModelAttemptHooks>();
    int reserved = 0, settled = 0;
    hooks->reserve = [&](const ModelAttemptContext &c,
                         ModelAttemptHooks::Completion done) {
      QVERIFY(!c.attemptId.isEmpty());
      QCOMPARE(client.calls, reserved);
      QCOMPARE(settled, reserved);
      ++reserved;
      done(Result<void, DomainError>::success());
    };
    hooks->settle = [&](const QString &, const LlmUsage &,
                        ModelAttemptHooks::Completion done) {
      ++settled;
      done(Result<void, DomainError>::success());
    };
    request.attemptHooks = hooks;
    bool completed = false;
    router.completeAsync(request, [&](auto r) {
      QVERIFY(r);
      completed = true;
    });
    QTRY_VERIFY(completed);
    QCOMPARE(client.calls, 2);
    QCOMPARE(reserved, 2);
    QCOMPARE(settled, 2);
    hooks->reserve = [&](const ModelAttemptContext &,
                         ModelAttemptHooks::Completion done) {
      done(Result<void, DomainError>::failure(
          domainError("STORE_UNAVAILABLE", "locked")));
    };
    completed = false;
    router.completeStreamAsync(request, {}, [&](auto r) {
      QVERIFY(!r);
      QCOMPARE(r.error().code, QString("STORE_UNAVAILABLE"));
      completed = true;
    });
    QTRY_VERIFY(completed);
    QCOMPARE(client.calls, 2);
  }
  void considerOpportunity_idleEnabledBudgeted_reservesOneTicket() {
    Fixture f;
    auto first = f.store.considerOpportunity(signal());
    QVERIFY(first);
    QVERIFY(!first.value().ticketId.isEmpty());
    auto duplicate = f.store.considerOpportunity(signal());
    QVERIFY(!duplicate);
    ActivityIntent i;
    i.goal = "read";
    i.context.profileId = profile;
    auto blocked = f.activities.startActivity(i);
    QVERIFY(blocked);
    QCOMPARE(blocked.value().state, ActivityState::Ready);
    QVERIFY(f.store.finishOpportunity(finish(first.value(), signal().clock)));
  }
  void considerOpportunity_paused_noModelCall() {
    Fixture f;
    QVERIFY(f.store.setPaused(true, signal().clock));
    auto r = f.store.considerOpportunity(signal());
    QVERIFY(!r);
    QCOMPARE(r.error().code, QString("OPPORTUNITY_DEFERRED"));
    QVERIFY(f.store.configure(f.config, signal().clock));
    QVERIFY(!f.store.considerOpportunity(signal()));
    QVERIFY(f.store.setPaused(false, signal().clock));
    QVERIFY(f.store.considerOpportunity(signal()));
  }
  void considerOpportunity_unchangedIdle_backsOffUntilImportantEvent() {
    Fixture f;
    auto first = f.store.considerOpportunity(signal());
    QVERIFY(first);
    QVERIFY(f.store.finishOpportunity(finish(first.value(), signal().clock)));
    QVERIFY(!f.store.considerOpportunity(signal(60000)));
    auto changed = signal(60000);
    changed.kind = "message";
    changed.contextId = "group-a";
    changed.inputWatermark = {{"group-a", "committed-1"}};
    changed.readiness.hostRevision = 2;
    auto second = f.store.considerOpportunity(changed);
    QVERIFY(second);
    QVERIFY(f.store.finishOpportunity(finish(second.value(), changed.clock)));
    changed.clock = signal(120000).clock;
    QVERIFY(!f.store.considerOpportunity(changed));
    changed.inputWatermark = {{"group-a", "committed-2"}};
    changed.readiness.hostRevision = 3;
    QVERIFY(f.store.considerOpportunity(changed));
  }
  void modelBudget_unknownUsageConservativeAndLateIdempotent() {
    Fixture f;
    auto t = f.store.considerOpportunity(signal());
    QVERIFY(t);
    ModelBudgetRequest r;
    r.profileId = profile;
    r.attemptId = "attempt-1";
    r.ticketId = t.value().ticketId;
    r.purpose = "selection";
    r.inputBytes = 100;
    r.maxOutputTokens = 1000;
    r.clock = signal().clock;
    auto reserved = f.store.reserveModelAttempt(r);
    QVERIFY(reserved);
    QCOMPARE(reserved.value().tokens, 2124);
    ModelBudgetSettlement s;
    s.profileId = profile;
    s.attemptId = r.attemptId;
    s.clock = r.clock;
    QVERIFY(f.store.settleModelAttempt(s));
    auto state = f.store.snapshot();
    QVERIFY(state);
    QCOMPARE(state.value()["hourlyBudget"]
                 .toObject()["attempts"]
                 .toObject()[r.attemptId]
                 .toObject()["tokens"]
                 .toInteger(),
             2124);
    s.usage.totalTokens = 2500;
    QVERIFY(f.store.settleModelAttempt(s));
    QVERIFY(f.store.settleModelAttempt(s));
    r.attemptId = "repair";
    QVERIFY(f.store.reserveModelAttempt(r));
    r.attemptId = "third";
    auto third = f.store.reserveModelAttempt(r);
    QVERIFY(!third);
    QCOMPARE(third.error().code, QString("BUDGET_EXHAUSTED"));
  }
  void
  finishOpportunity_waitCancelExpiryAndRestart_preservesChargedUsageAndRejectsStale() {
    Fixture f;
    auto t = f.store.considerOpportunity(signal());
    QVERIFY(t);
    auto late = finish(t.value(), signal(120001).clock);
    QVERIFY(!f.store.finishOpportunity(late));
    auto next = f.store.considerOpportunity(signal(120001));
    QVERIFY(next);
    auto restarted = signal(180001);
    restarted.clock.sessionId = "run-2";
    restarted.clock.monotonicMs = 0;
    QVERIFY(f.store.configure(f.config, restarted.clock));
    QVERIFY(!f.store.finishOpportunity(finish(next.value(), restarted.clock)));
  }
  void evaluateIdle_onlyExternalWaitOrBackgroundMemory_returnsIdle() {
    // WaitingExternal and background maintenance do not own foreground.
    RuntimeReadiness readiness;
    readiness.running = true;
    readiness.autonomyEnabled = true;
    const auto decision = evaluateIdle(readiness);
    QVERIFY(decision.isIdle);
    QVERIFY(decision.canDeliberate);
    QVERIFY(decision.blockingRefs.isEmpty());
  }

  void evaluateIdle_toolWaitValidationOrReadyObligation_returnsBusy() {
    RuntimeReadiness readiness;
    readiness.running = true;
    readiness.autonomyEnabled = true;
    readiness.foregroundActivity = "reading-between-steps";
    auto decision = evaluateIdle(readiness);
    QVERIFY(!decision.isIdle);
    QVERIFY(!decision.canDeliberate);
    QVERIFY(decision.blockingRefs.contains(readiness.foregroundActivity));
    readiness.foregroundActivity.clear();
    readiness.selectionReservation = "selecting";
    QVERIFY(!evaluateIdle(readiness).isIdle);
    readiness.selectionReservation.clear();
    readiness.readyObligations = {"reminder-due"};
    QVERIFY(!evaluateIdle(readiness).isIdle);
    readiness.readyObligations.clear();
    readiness.pendingResults = {"verify-action"};
    QVERIFY(!evaluateIdle(readiness).isIdle);
  }

  void evaluateIdle_controlsRemainSeparateFromIdleness() {
    RuntimeReadiness readiness;
    readiness.running = true;
    readiness.autonomyEnabled = true;
    readiness.paused = true;
    auto decision = evaluateIdle(readiness);
    QVERIFY(decision.isIdle);
    QVERIFY(!decision.canDeliberate);
    QCOMPARE(decision.reason, QString("paused"));
    readiness.paused = false;
    readiness.sleeping = true;
    QVERIFY(!evaluateIdle(readiness).canDeliberate);
    readiness.sleeping = false;
    readiness.autonomyEnabled = false;
    QVERIFY(!evaluateIdle(readiness).canDeliberate);
  }
};

QTEST_GUILESS_MAIN(TestActivityOpportunities)
#include "test_activity_opportunities.moc"
