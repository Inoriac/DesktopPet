#include "ai/activity/activity_runtime.h"
#include "ai/activity/opportunity_store.h"
#include "ai/ai_brain.h"
#include "ai/observation/observation_store.h"
#include "ai/runtime/agent_bootstrap.h"
#include "ai/runtime/agent_runtime_services.h"
#include "ai/runtime/runtime_ui_bridge.h"
#include "controlled_chat_fixture.h"
#include <QPointer>
#include <QtTest>
using namespace ControlledChat;
namespace {
class Client final : public ModelCompletionClient {
public:
  std::function<std::optional<QJsonObject>(QString, QJsonObject)> answer;
  LlmCompletionHandler deferred;
  QList<QJsonObject> inputs;
  void completeOnce(const ModelRouteConfig &,
                    const QList<ChatMessage> &messages, const QJsonArray &,
                    LlmCompletionHandler done, const QString &) override {
    auto input =
        QJsonDocument::fromJson(messages.last().content.toUtf8()).object();
    inputs.append(input);
    auto output = answer(messages.first().content, input);
    if (!output) {
      deferred = std::move(done);
      return;
    }
    LlmResponse response;
    response.content = QString::fromUtf8(
        QJsonDocument(*output).toJson(QJsonDocument::Compact));
    done(true, response, {});
  }
  void release(QJsonObject output) {
    auto done = std::move(deferred);
    LlmResponse response;
    response.content =
        QString::fromUtf8(QJsonDocument(output).toJson(QJsonDocument::Compact));
    done(true, response, {});
  }
};
QList<ModelRoleConfig> roles() {
  ModelRouteConfig route;
  route.routeId = "controlled";
  route.enabled = route.llm.enabled = true;
  route.llm.provider = "controlled";
  route.llm.baseUrl = "https://invalid.test";
  route.llm.apiKey = "fixture";
  route.llm.model = "controlled";
  route.llm.timeoutMs = 10000;
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
QJsonObject choice(QString decision, QString goal = {}, CapabilityRef ref = {},
                   QString id = {}) {
  return {{"decision", decision},
          {"reason", "Use the current evidence and continue the activity"},
          {"evidenceRefs", QJsonArray{}},
          {"goal", goal},
          {"stopWhen", "Actual intended result is verified"},
          {"reconsiderWhen", "A matching reply arrives"},
          {"capabilityRefs",
           ref.pluginId.isEmpty() ? QJsonArray{} : QJsonArray{ref.toJson()}},
          {"candidates", QJsonArray{}},
          {"activityId", id},
          {"nextConsiderAt", ""}};
}
QJsonObject plan() {
  return {{"required_source_tools", QJsonArray{"invoke_capability"}},
          {"plan", "Continue from the saved page or message"},
          {"completion_criteria", "Actual result provides evidence"}};
}
QJsonObject tool(QString name, CapabilityRef ref, QJsonObject args = {}) {
  auto p = plan();
  QJsonObject a{{"ref", ref.toJson()}};
  if (name == "invoke_capability")
    a["arguments"] = args;
  p["tool"] = QJsonObject{{"name", name}, {"arguments", a}};
  return p;
}
QJsonObject verify(const QJsonArray &steps) {
  return {{"complete", true},
          {"supported", true},
          {"contradictions", QJsonArray{}},
          {"unresolved", QJsonArray{}},
          {"evidence", QJsonArray{steps.last().toObject()["id"]}},
          {"result", "Verified the actual result"},
          {"reason", "Actual committed capability evidence"}};
}
CapabilityRef addReader(Fixture &f, bool renamed) {
  const auto op = renamed ? QString("study_leaf") : QString("read_page");
  QFile reader(f.root + "/reader.py");
  if (reader.open(QIODevice::WriteOnly))
    reader.write("import "
                 "json,sys\nv=json.load(sys.stdin)\np=v['page']\nprint(json."
                 "dumps({'page':p,'text':{1:'First chapter: shared "
                 "context',2:'Second chapter: durable continuity'}[p]}))\n");
  reader.close();
  auto t = readJson(f.root + "/tool.json");
  auto d = t["definition"].toObject();
  auto configs = d["configuration"].toArray();
  configs.append(QJsonObject{{"key", "reader"},
                             {"kind", "path"},
                             {"required", true},
                             {"description", "Read bounded pages"}});
  d["configuration"] = configs;
  auto ops = d["operations"].toArray();
  auto operation = ops[1].toObject();
  operation["id"] = op;
  operation["description"] = "Read one document page";
  operation["parameters"] = QJsonObject{
      {"source", "inline"},
      {"schema",
       QJsonObject{
           {"type", "object"},
           {"properties", QJsonObject{{"page", QJsonObject{{"type", "integer"},
                                                           {"minimum", 1},
                                                           {"maximum", 2}}}}},
           {"required", QJsonArray{"page"}},
           {"additionalProperties", false}}}};
  auto invocation = operation["invocation"].toObject();
  invocation["arguments"] = QJsonArray{QJsonObject{{"setting", "reader"}}};
  operation["invocation"] = invocation;
  ops.append(operation);
  d["operations"] = ops;
  t["definition"] = d;
  writeJson(f.root + "/tool.json", t);
  auto settings = f.configuration["configuration"].toObject();
  auto entry = settings[f.entry].toObject();
  entry["reader"] = f.root + "/reader.py";
  settings[f.entry] = entry;
  f.configuration["configuration"] = settings;
  auto grants = f.configuration["grants"].toArray();
  grants.append(QJsonObject{{"id", "internal-book"},
                            {"profileId", profile},
                            {"pluginId", f.plugin},
                            {"entryId", f.entry},
                            {"operationId", op},
                            {"scope", "internal_activity"},
                            {"effects", QJsonArray{"read"}},
                            {"executor", testPythonExecutable()},
                            {"autonomous", true}});
  f.configuration["grants"] = grants;
  return f.ref(op);
}
} // namespace
class AutonomousChatTests : public QObject {
  Q_OBJECT
private slots:
  void
  externalCue_busyPauseRestartAndInternalTick_preservesPendingConversationAndEvents() {
    Fixture f;
    ObservationStore observations(profile, f.path);
    auto accepted = observations.ingest(f.binding(), f.envelope());
    QVERIFY(accepted);
    OpportunityStore opportunities(profile, f.path);
    AutonomyConfig config;
    config.enabled = true;
    config.minDecisionIntervalMs = 1;
    auto clock = OpportunityStore::systemClock();
    QVERIFY(opportunities.configure(config, clock));
    OpportunitySignal signal;
    signal.kind = "tick";
    signal.clock = clock;
    signal.readiness.running = true;
    signal.readiness.autonomyEnabled = true;
    signal.readiness.hostRevision = 1;
    signal.readiness.modelPending = true;
    QVERIFY(!opportunities.considerOpportunity(signal));
    signal.readiness.modelPending = false;
    QVERIFY(opportunities.setPaused(true, clock));
    QVERIFY(!opportunities.considerOpportunity(signal));
    // A new host process/lane reads the same durable original conversation cue.
    OpportunityStore restarted(profile, f.path);
    QVERIFY(restarted.setPaused(false, clock));
    signal.clock.utc = clock.utc.addSecs(2);
    signal.clock.monotonicMs += 2000;
    signal.clock.sessionId = "restarted-host";
    auto ticket = restarted.considerOpportunity(signal);
    if (!ticket)
      QFAIL(qPrintable(ticket.error().code + ": " + ticket.error().message));
    QCOMPARE(ticket.value().contextId, accepted.value().message.conversationId);
    QCOMPARE(ticket.value().eventIds,
             QStringList{accepted.value().message.originId});
    OpportunityFinish finish;
    finish.ticketId = ticket.value().ticketId;
    finish.hostRevision = ticket.value().hostRevision;
    finish.inputWatermark = ticket.value().inputWatermark;
    finish.clock = signal.clock;
    finish.outcome = "wait";
    QVERIFY(restarted.finishOpportunity(finish));
    auto u = SqliteRuntimeUnitOfWork::begin(f.path);
    QVERIFY(u);
    auto cue = ObservationStore::pendingCue(
        QSqlDatabase::database(u.value()->connectionName()), profile);
    QVERIFY(cue);
    QVERIFY(!cue.value());
  }
  void
  autonomousChat_checkDiscussWaitReadAndResume_preservesPeopleAndContexts_data() {
    QTest::addColumn<bool>("renamed");
    QTest::newRow("original") << false;
    QTest::newRow("replacement-package-and-operations") << true;
  }
  void
  autonomousChat_checkDiscussWaitReadAndResume_preservesPeopleAndContexts() {
    QFETCH(bool, renamed);
    Fixture f(renamed);
    QVERIFY(f.valid);
    auto reading = addReader(f, renamed);
    QVERIFY(
        !f.inject("alice", "Shall we discuss the first chapter?").isEmpty());
    Client client;
    AIBrain brain(&client, roles());
    ToolRegistry registry;
    CallbackRuntimeUiBridge bridge(RuntimeUiCallbacks{});
    AgentRuntimeServices services;
    RuntimeStartRequest start;
    start.profile = {"ControlledPet", "model.gltf", profile};
    start.profileMigration.profileId = profile;
    start.profileMigration.registeredProfileIds = {profile};
    start.profileMigration.appDataRoot = f.directory.filePath("agent");
    start.profileMigration.legacyDatabasePath =
        f.directory.filePath("legacy.db");
    start.profileMigration.legacyJsonPath = f.directory.filePath("legacy.json");
    start.configHash = QString(64, 'a');
    start.identityBaselineHash = QString(64, 'b');
    start.aiBrain = &brain;
    start.uiBridge = &bridge;
    QVERIFY(AgentBootstrap::start(services, start));
    brain.setEnabled(true);
    brain.setToolRegistry(&registry);
    QVERIFY(brain.pluginRuntime());
    f.path = services.chatPreparationRuntimeMetadata().runtimeDatabasePath;
    QVERIFY(brain.pluginRuntime()->setHostConfiguration(f.configuration));
    int selections = 0;
    bool talkResumed = false, readingResumed = false, held = false,
         startReading = false;
    QString talkId, readingId;
    QJsonObject heldPlan;
    client.answer = [&](QString system,
                        QJsonObject input) -> std::optional<QJsonObject> {
      if (system.startsWith("Choose a worthwhile autonomous")) {
        ++selections;
        const auto context = input["context"].toObject();
        const auto activities = input["activities"].toArray();
        for (const auto &value : activities) {
          const auto row = value.toObject();
          if ((row["state"] == "Ready" || row["state"] == "Paused") &&
              (row["goal"] == "talk" || row["goal"] == "reading")) {
            if (row["goal"] == "talk") {
              talkResumed = true;
              talkId = row["id"].toString();
            } else {
              readingResumed = true;
              readingId = row["id"].toString();
            }
            return choice("resume", {}, {}, row["id"].toString());
          }
        }
        if (context["originKind"] == "external") {
          if (context["conversationId"] !=
              conversationIdFor(profile, "controlled-chat", "account-one",
                                "group-a"))
            return choice("wait");
          return choice("start", "talk", f.ref(f.send));
        }
        if (selections == 1)
          return choice("start", "check", f.ref(f.read));
        if (startReading && !held)
          return choice("start", "reading", reading);
        return choice("wait");
      }
      if (system.startsWith("You are the companion's subjective"))
        return QJsonObject{{"decision", "accept"},
                           {"reason", "Interested in the actual material"},
                           {"message", "Continue"}};
      const auto steps = input["steps"].toArray();
      const auto goal = input["goal"].toString();
      if (system.startsWith("Plan one next step")) {
        const auto ref = goal == "reading" ? reading
                         : goal == "talk"  ? f.ref(f.send)
                                           : f.ref(f.read);
        if (steps.isEmpty())
          return tool("load_capability", ref);
        if (steps.size() == 1)
          return tool(
              "invoke_capability", ref,
              goal == "reading" ? QJsonObject{{"page", 1}}
              : goal == "talk"
                  ? QJsonObject{{"chat_id", "group-a"},
                                {"text",
                                 "What did you think of the first chapter?"},
                                {"reply_to", "message-1"}}
                  : QJsonObject{{"chat_id", "group-a"}});
        if (goal == "talk" && !talkResumed) {
          auto p = plan();
          p["wait"] =
              QJsonObject{{"kind", "message"},
                          {"afterStepId", steps.last().toObject()["id"]},
                          {"reason", "Wait for a real reply"}};
          return p;
        }
        if (goal == "reading" && steps.size() == 2) {
          auto p = tool("invoke_capability", reading, {{"page", 2}});
          if (!held) {
            held = true;
            heldPlan = p;
            return std::nullopt;
          }
          return p;
        }
        auto p = plan();
        p["candidate_answer"] = "The actual source has been processed";
        return p;
      }
      if (system.startsWith("Independently verify"))
        return verify(steps);
      return QJsonObject{};
    };
    AutonomyConfig autonomy;
    autonomy.enabled = true;
    autonomy.minDecisionIntervalMs = 1;
    autonomy.localTickMs = 50;
    autonomy.noOpBackoffMs = 50;
    autonomy.maxNoOpBackoffMs = 100;
    autonomy.maxSelectionCallsPerHour = 100;
    autonomy.maxTokensPerHour = 10000000;
    autonomy.maxEstimatedCostMicrosPerHour = 100000000;
    brain.setAutonomyConfig(autonomy);
    brain.start();
    ActivityStore activities(profile, f.path);
    QTRY_VERIFY_WITH_TIMEOUT(
        !activities.list({ActivityState::WaitingExternal}).value().isEmpty(),
        15000);
    auto waiting = activities.list({ActivityState::WaitingExternal});
    QVERIFY(waiting);
    QCOMPARE(waiting.value().size(), 1);
    talkId = waiting.value().first().activityId;
    auto sent =
        waiting.value().first().waitingCondition["afterMessageId"].toString();
    QVERIFY(!sent.isEmpty());
    QCOMPARE(f.count("agent_messages", "direction='outgoing'"), 1);
    QCOMPARE(f.count("agent_response_dispatches", "status='completed'"), 1);
    // Source was absent from the initial query path; enabling it replays the
    // same journal and must not create a second Alice message or send.
    brain.pluginRuntime()->setObservationSource(
        {"account", true, {}, brain.pluginRuntime()->catalogRevision()},
        [](auto) {});
    startReading = true;
    QTRY_VERIFY_WITH_TIMEOUT(held && bool(client.deferred), 15000);
    const auto beforeReading = brain.taskSnapshot();
    readingId = beforeReading["id"].toString();
    QCOMPARE(beforeReading["goal"].toString(), QString("reading"));
    QVERIFY(!f.inject("bob", "An unrelated group topic", "group-b").isEmpty());
    QTRY_COMPARE(f.count("agent_messages", "direction='incoming'"), 2);
    QTest::qWait(100);
    QCOMPARE(brain.taskSnapshot()["id"].toString(), readingId);
    QVERIFY(brain.isTaskActive());
    QVERIFY(!f.inject("alice", "I liked it; continue your reading", "group-a",
                      "message-2")
                 .isEmpty());
    QTRY_COMPARE(f.count("agent_messages", "direction='incoming'"), 3);
    QTRY_VERIFY_WITH_TIMEOUT(
        activities.find(talkId).value().state == ActivityState::Ready, 5000);
    client.release(heldPlan);
    QTRY_VERIFY_WITH_TIMEOUT(talkResumed, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(activities.find(talkId).value().state ==
                                 ActivityState::Completed,
                             10000);
    QTRY_VERIFY_WITH_TIMEOUT(readingResumed, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(activities.find(readingId).value().state ==
                                 ActivityState::Completed,
                             10000);
    const auto readDone = activities.task(readingId);
    QVERIFY(readDone);
    QCOMPARE(readDone.value().toolCalls, 3);
    QVERIFY(readDone.value().activeMs >=
            beforeReading["active_ms"].toInteger());
    QCOMPARE(f.count("agent_messages", "direction='outgoing'"), 1);
    const auto talk = activities.task(talkId);
    QVERIFY(talk);
    QCOMPARE(talk.value().context.originKind, OriginKind::External);
    QCOMPARE(talk.value().context.participantPersonIds.size(), 1);
    QVERIFY(!talk.value().context.participantPersonIds.contains("owner"));
    QCOMPARE(readDone.value().context.originKind, OriginKind::Internal);
    QVERIFY(readDone.value().context.participantPersonIds.isEmpty());
    brain.stop();
  }
};
QTEST_GUILESS_MAIN(AutonomousChatTests)
#include "test_autonomous_chat.moc"
