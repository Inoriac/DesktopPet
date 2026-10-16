#include "ai/activity/opportunity_store.h"
#include "ai/chat/conversation_service.h"
#include "ai/chat/conversation_store.h"
#include "ai/observation/observation_store.h"
#include "ai/plugin/schema_validator.h"
#include "controlled_chat_fixture.h"
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
using namespace ControlledChat;

class ExternalObservationTests : public QObject {
  Q_OBJECT
private slots:
  void ingestObservation_pollAndPushSameMessage_oneMessageAndOrigin() {
    Fixture f;
    QVERIFY(f.valid);
    ObservationStore store(profile, f.path);
    auto event = f.envelope();
    auto first = store.ingest(f.binding(), event);
    if (!first)
      QFAIL(qPrintable(first.error().code + ": " + first.error().message));
    event.payload["occurredAt"] = "2026-10-16T10:00:00Z";
    auto replay = store.ingest(f.binding(), event);
    QVERIFY(replay);
    QVERIFY(replay.value().duplicate);
    QCOMPARE(first.value().message.messageId, replay.value().message.messageId);
    QCOMPARE(first.value().message.originId, replay.value().message.originId);
    QCOMPARE(f.count("agent_messages"), 1);
    QCOMPARE(f.count("event_log", "type='MessageObserved'"), 1);
    QCOMPARE(store.cursor(f.binding()).value(), QString("cursor:1"));
  }
  void sameRemoteIdAcrossGroups_isolated() {
    Fixture f;
    ObservationStore store(profile, f.path);
    auto a = store.ingest(f.binding(), f.envelope());
    QVERIFY(a);
    auto b = store.ingest(f.binding(), f.envelope("2", "group-b"));
    QVERIFY(b);
    QVERIFY(a.value().message.messageId != b.value().message.messageId);
    QCOMPARE(f.count("agent_messages"), 2);
  }
  void
  ingestObservation_messageOutboxCursorFailure_rollsBackAndDoesNotAck_data() {
    QTest::addColumn<QString>("table");
    QTest::newRow("message") << QString("agent_messages");
    QTest::newRow("outbox") << QString("event_outbox");
    QTest::newRow("marker") << QString("agent_runtime_migrations");
    QTest::newRow("cursor") << QString("agent_runtime_state");
  }
  void ingestObservation_messageOutboxCursorFailure_rollsBackAndDoesNotAck() {
    QFETCH(QString, table);
    Fixture f;
    ObservationStore store(profile, f.path);
    // Resolve the speaker first so identity outbox is independent of the
    // message fault.
    auto warm = f.envelope("1");
    QVERIFY(store.ingest(f.binding(), warm));
    QVERIFY(f.sql("CREATE TRIGGER reject_fact BEFORE INSERT ON " + table +
                  " BEGIN SELECT RAISE(FAIL,'injected'); END"));
    if (table == "agent_runtime_state")
      QVERIFY(f.sql(
          "CREATE TRIGGER reject_update BEFORE UPDATE ON agent_runtime_state "
          "BEGIN SELECT RAISE(FAIL,'injected'); END"));
    auto result =
        store.ingest(f.binding(), f.envelope("2", "group-a", "message-2"));
    QVERIFY(!result);
    QCOMPARE(result.error().code, QString("STORE_UNAVAILABLE"));
    QCOMPARE(f.count("agent_messages"), 1);
    QCOMPARE(store.cursor(f.binding()).value(), QString("cursor:1"));
    QVERIFY(f.sql("DROP TRIGGER reject_fact"));
    if (table == "agent_runtime_state")
      QVERIFY(f.sql("DROP TRIGGER reject_update"));
    // Another transport cannot leap past the uncommitted journal fact.
    QVERIFY(
        !store.ingest(f.binding(), f.envelope("3", "group-a", "message-3")));
    QVERIFY(store.ingest(f.binding(), f.envelope("2", "group-a", "message-2")));
    QVERIFY(store.ingest(f.binding(), f.envelope("3", "group-a", "message-3")));
    QCOMPARE(store.cursor(f.binding()).value(), QString("cursor:3"));
  }
  void ingestObservation_sequenceConflictAndLateReplay_doesNotRollBackCursor() {
    Fixture f;
    ObservationStore store(profile, f.path);
    QVERIFY(
        store.ingest(f.binding(), f.envelope("10", "group-a", "message-10")));
    QVERIFY(store.ingest(f.binding(), f.envelope("2", "group-a", "message-2")));
    QCOMPARE(store.cursor(f.binding()).value(), QString("cursor:10"));
    auto conflict = f.envelope("2", "group-a", "another-message");
    QVERIFY(!store.ingest(f.binding(), conflict));
    for (const auto &invalid :
         {QString("01"), QString("1\n"), QString("9223372036854775808"),
          QString("0"), QString("1e2")})
      QVERIFY(!store.ingest(f.binding(), f.envelope(invalid)));
    auto number = f.envelope("3");
    number.payload["sequence"] = 3;
    QVERIFY(!store.ingest(f.binding(), number));
    auto changed = f.binding();
    changed.connectionKey = "new-account";
    for (auto &c : changed.conversations)
      c.connectionKey = changed.connectionKey;
    QCOMPARE(store.cursor(changed).value(), QString());
    QVERIFY(store.ingest(changed, f.envelope("1")));
  }
  void outboxDeliveryDelay_preservesCueUntilOriginalEventExists() {
    Fixture f;
    ObservationStore store(profile, f.path);
    QVERIFY(f.sql("CREATE TRIGGER delayed BEFORE INSERT ON event_log BEGIN "
                  "SELECT RAISE(FAIL,'offline'); END"));
    auto accepted = store.ingest(f.binding(), f.envelope());
    QVERIFY(accepted);
    {
      auto unit = SqliteRuntimeUnitOfWork::begin(f.path);
      QVERIFY(unit);
      auto cue = ObservationStore::pendingCue(
          QSqlDatabase::database(unit.value()->connectionName()), profile);
      QVERIFY(cue);
      QVERIFY(!cue.value());
    }
    QVERIFY(f.sql("DROP TRIGGER delayed"));
    ConversationStore messages(profile, f.path);
    QVERIFY(messages.recoverOutbox());
    for (int i = 2; i <= 12; i++)
      QVERIFY(store.ingest(f.binding(),
                           f.envelope(QString::number(i), "group-a",
                                      "message-" + QString::number(i))));
    QTRY_COMPARE((messages.recoverOutbox(),
                  f.count("event_log", "type='MessageObserved'")),
                 12);
    {
      auto unit = SqliteRuntimeUnitOfWork::begin(f.path);
      auto db = QSqlDatabase::database(unit.value()->connectionName());
      auto cue = ObservationStore::pendingCue(db, profile);
      QVERIFY(cue);
      QVERIFY(cue.value());
      QCOMPARE(cue.value()->eventIds.size(), 8);
      QVERIFY(
          ObservationStore::consumeCues(db, profile, cue.value()->eventIds));
      QVERIFY(unit.value()->commit());
    }
    {
      auto unit = SqliteRuntimeUnitOfWork::begin(f.path);
      auto cue = ObservationStore::pendingCue(
          QSqlDatabase::database(unit.value()->connectionName()), profile);
      QVERIFY(cue);
      QVERIFY(cue.value());
      QCOMPARE(cue.value()->eventIds.size(), 4);
    }
  }
  void pollResult_exactCompletedMappingOnly_normalizesMessages_data() {
    QTest::addColumn<bool>("withSource");
    QTest::newRow("source-disabled") << true;
    QTest::newRow("package-without-sources") << false;
  }
  void pollResult_exactCompletedMappingOnly_normalizesMessages() {
    QFETCH(bool, withSource);
    Fixture f(false, withSource);
    QVERIFY(f.valid);
    QVERIFY(!f.inject().isEmpty());
    PluginRuntime runtime(profile, f.directory.filePath("plugins.json"));
    runtime.setObservationDatabase(f.path);
    QVERIFY(runtime.setHostConfiguration(f.configuration));
    ContextSpec context;
    context.profileId = profile;
    auto loaded = runtime.loadCapability(f.ref(f.read), context, true);
    QVERIFY(loaded);
    ToolExecutionRequest request;
    request.requestId = "poll-one";
    request.activityId = "check-one";
    request.context = context;
    request.autonomous = true;
    request.toolName = "invoke_capability";
    request.arguments = {{"ref", f.ref(f.read).toJson()},
                         {"arguments", QJsonObject{{"chat_id", "group-a"}}}};
    std::optional<ToolExecutionOutcome> result;
    runtime.executeGateway(request, [&](auto out) { result = out; });
    QTRY_VERIFY(result);
    QVERIFY2(result->result.success, qPrintable(result->result.errorMessage));
    QCOMPARE(f.count("agent_messages"), 1);
    auto config = ObservationConfiguration::parse(f.configuration);
    QVERIFY(config);
    const auto adapter = config.value().adapters.first();
    CapabilityOutcome ordinary;
    ordinary.stage = "accepted";
    ordinary.data = QJsonObject{{"items", QJsonArray{f.envelope().payload}}};
    QVERIFY(ObservationStore::normalize(adapter, f.binding(), ordinary)
                .value()
                .isEmpty());
    ordinary.stage = "completed";
    ordinary.data = QJsonObject{{"items", QJsonArray{}}};
    QVERIFY(ObservationStore::normalize(adapter, f.binding(), ordinary)
                .value()
                .isEmpty());
    QJsonArray oversized;
    for (int i = 0; i < 257; i++)
      oversized.append(QJsonObject{});
    ordinary.data = QJsonObject{{"items", oversized}};
    QVERIFY(!ObservationStore::normalize(adapter, f.binding(), ordinary));
  }
  void setObservationSource_enableReconnectDisable_preservesCommittedCursor() {
    Fixture f;
    QVERIFY(f.valid);
    QVERIFY(!f.inject().isEmpty());
    PluginRuntime runtime(profile, f.directory.filePath("plugins.json"));
    runtime.setObservationDatabase(f.path);
    QVERIFY(runtime.setHostConfiguration(f.configuration));
    std::optional<Result<SourceState, DomainError>> enabled;
    runtime.setObservationSource(
        {"account", true, {}, runtime.catalogRevision()},
        [&](auto r) { enabled = r; });
    QVERIFY(enabled);
    QVERIFY(*enabled);
    QTRY_COMPARE(f.count("agent_messages"), 1);
    std::optional<Result<SourceState, DomainError>> disabled;
    runtime.setObservationSource(
        {"account", false, {}, runtime.catalogRevision()},
        [&](auto r) { disabled = r; });
    QVERIFY(disabled);
    QVERIFY(*disabled);
    QVERIFY(!f.inject("bob", "Second actual message").isEmpty());
    QTest::qWait(150);
    QCOMPARE(f.count("agent_messages"), 1);
    runtime.setObservationSource(
        {"account", true, {}, runtime.catalogRevision()},
        [&](auto r) { enabled = r; });
    QTRY_COMPARE(f.count("agent_messages"), 2);
    QCOMPARE(ObservationStore(profile, f.path).cursor(f.binding()).value(),
             QString("cursor:2"));
    runtime.setObservationSource(
        {"account", false, {}, runtime.catalogRevision()}, [](auto) {});
  }
  void partialMultipleLinesBoundsAndRetiredGeneration_noUncommittedAck_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("count");
    QTest::newRow("split-lines") << QString("split") << 2;
    QTest::newRow("invalid-first-line") << QString("invalid") << 0;
    QTest::newRow("overlong-first-line") << QString("overlong") << 0;
    QTest::newRow("queue-overflow") << QString("burst") << 256;
  }
  void partialMultipleLinesBoundsAndRetiredGeneration_noUncommittedAck() {
    QFETCH(QString, mode);
    QFETCH(int, count);
    Fixture f;
    QJsonArray events;
    const int total = mode == "burst" ? 300 : 2;
    for (int i = 1; i <= total; i++)
      events.append(f.envelope(QString::number(i), "group-a",
                               "message-" + QString::number(i))
                        .payload);
    QVERIFY(f.sourceProbe(events, mode));
    PluginRuntime runtime(profile, f.directory.filePath("plugins.json"));
    runtime.setObservationDatabase(f.path);
    QStringList errors;
    int admittedAtOverflow = -1;
    connect(&runtime, &PluginRuntime::observationSourceState, this,
            [&](SourceState state) {
              if (state.error == "SOURCE_QUEUE_LIMIT")
                admittedAtOverflow =
                    state.cursor.section(':', -1).toInt() + 256;
              if (!state.error.isEmpty())
                errors.append(state.error);
            });
    QVERIFY(runtime.setHostConfiguration(f.configuration));
    if (mode == "burst") {
      QTRY_VERIFY_WITH_TIMEOUT(errors.contains("SOURCE_QUEUE_LIMIT"), 5000);
      QTRY_COMPARE_WITH_TIMEOUT(f.count("agent_messages"), admittedAtOverflow,
                                15000);
      QVERIFY(admittedAtOverflow >= 256);
      QVERIFY(f.count("agent_messages") < total);
    } else if (count)
      QTRY_COMPARE_WITH_TIMEOUT(f.count("agent_messages"), count, 5000);
    else {
      QTRY_VERIFY(!errors.isEmpty());
      QCOMPARE(f.count("agent_messages"), 0);
    }
    if (mode != "split")
      QVERIFY(!errors.isEmpty());
    const auto revision = runtime.catalogRevision();
    runtime.setObservationSource({"account", false, {}, revision}, [](auto) {});
    std::optional<Result<SourceState, DomainError>> stale;
    runtime.setObservationSource({"account", true, {}, revision},
                                 [&](auto result) { stale = result; });
    QVERIFY(stale);
    QVERIFY(!*stale);
    QFile log(f.directory.filePath("source-log.jsonl"));
    QVERIFY(log.open(QIODevice::ReadOnly));
    const auto lines = log.readAll();
    QVERIFY(!lines.contains("event-300"));
    if (count == 0)
      QVERIFY(!lines.contains("\"type\":\"ack\""));
  }
  void failedFirstSourceFact_blocksSecondAndDrainsAfterFacadeDestruction() {
    Fixture f;
    QVERIFY(f.sourceProbe(
        {f.envelope().payload, f.envelope("2", "group-a", "message-2").payload},
        "split"));
    QVERIFY(f.sql("CREATE TRIGGER blocked BEFORE INSERT ON agent_messages "
                  "BEGIN SELECT RAISE(FAIL,'offline'); END"));
    auto runtime = std::make_unique<PluginRuntime>(
        profile, f.directory.filePath("plugins.json"));
    runtime->setObservationDatabase(f.path);
    QString error;
    connect(runtime.get(), &PluginRuntime::observationSourceState, this,
            [&](SourceState state) { error = state.error; });
    QVERIFY(runtime->setHostConfiguration(f.configuration));
    QTRY_COMPARE(error, QString("STORE_UNAVAILABLE"));
    QCOMPARE(f.count("agent_messages"), 0);
    QCOMPARE(ObservationStore(profile, f.path).cursor(f.binding()).value(),
             QString());
    QElapsedTimer clock;
    clock.start();
    runtime.reset();
    QVERIFY(clock.elapsed() < 1000);
    QVERIFY(f.sql("DROP TRIGGER blocked"));
    QTRY_COMPARE_WITH_TIMEOUT(f.count("agent_messages"), 2, 5000);
    QCOMPARE(ObservationStore(profile, f.path).cursor(f.binding()).value(),
             QString("cursor:2"));
  }
  void
  sourceContract_invalidDefinitionConfigurationPathAndExecutor_rejectsBeforeStart_data() {
    QTest::addColumn<QString>("problem");
    for (const auto &name :
         {"protocol", "model-argument", "undeclared-setting",
          "optional-program", "outside-path", "executor", "configuration-type"})
      QTest::newRow(name) << QString(name);
  }
  void
  sourceContract_invalidDefinitionConfigurationPathAndExecutor_rejectsBeforeStart() {
    QFETCH(QString, problem);
    Fixture f;
    auto source = readJson(f.root + "/source.json");
    auto invocation = source["invocation"].toObject();
    if (problem == "protocol")
      source["protocol"] = "invented";
    if (problem == "model-argument")
      invocation["arguments"] = QJsonArray{QJsonObject{{"input", "/command"}}};
    if (problem == "undeclared-setting")
      invocation["program"] = QJsonObject{{"setting", "unknown"}};
    if (problem == "optional-program") {
      auto declarations = source["configuration"].toArray();
      auto program = declarations[0].toObject();
      program["required"] = false;
      declarations[0] = program;
      source["configuration"] = declarations;
    }
    source["invocation"] = invocation;
    QVERIFY(writeJson(f.root + "/source.json", source));
    if (problem == "outside-path") {
      auto manifest = readJson(f.root + "/plugin.json");
      auto sources = manifest["sources"].toArray();
      auto entry = sources[0].toObject();
      entry["definition"] = "../source.json";
      sources[0] = entry;
      manifest["sources"] = sources;
      QVERIFY(writeJson(f.root + "/plugin.json", manifest));
    }
    if (problem == "executor" || problem == "configuration-type") {
      auto bindings = f.configuration["observationBindings"].toArray();
      auto b = bindings[0].toObject();
      auto control = b["source"].toObject();
      control["enabled"] = true;
      if (problem == "executor")
        control["executor"] = "/bin/sh";
      b["source"] = control;
      bindings[0] = b;
      f.configuration["observationBindings"] = bindings;
      if (problem == "configuration-type") {
        auto configs = f.configuration["configuration"].toObject();
        auto settings = configs[f.source].toObject();
        settings["program"] = 3;
        configs[f.source] = settings;
        f.configuration["configuration"] = configs;
      }
    }
    PluginRuntime runtime(profile, f.directory.filePath("plugins.json"));
    runtime.setObservationDatabase(f.path);
    QVERIFY(!runtime.setHostConfiguration(f.configuration));
    QCOMPARE(f.count("agent_messages"), 0);
  }
  void
  autonomousSend_actualReceiptArchivedBeforeWaiting_keepsOriginalScope_data() {
    QTest::addColumn<QString>("mode");
    for (const auto &mode : {"plain", "normalized", "secret"})
      QTest::newRow(mode) << QString(mode);
  }
  void autonomousSend_actualReceiptArchivedBeforeWaiting_keepsOriginalScope() {
    QFETCH(QString, mode);
    Fixture f;
    const QString requested =
        mode == "secret" ? "An actual reply fixture-secret" : "An actual reply";
    const QString actual =
        mode == "normalized" ? requested + " normalized" : requested;
    if (mode == "normalized") {
      QFile file(f.root + "/adapter.py");
      QVERIFY(file.open(QIODevice::ReadOnly));
      auto body = file.readAll();
      file.close();
      body.replace("request['text'], request.get('reply_to', '')",
                   "request['text'] + (' normalized' if args.operation == "
                   "'send' else ''), request.get('reply_to', '')");
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(body);
    }
    if (mode == "secret") {
      auto t = readJson(f.root + "/tool.json");
      auto d = t["definition"].toObject();
      auto declarations = d["configuration"].toArray();
      declarations.append(QJsonObject{{"key", "credential"},
                                      {"kind", "secret"},
                                      {"required", true},
                                      {"description", "Redaction fixture"}});
      d["configuration"] = declarations;
      t["definition"] = d;
      QVERIFY(writeJson(f.root + "/tool.json", t));
      auto configs = f.configuration["configuration"].toObject();
      auto settings = configs[f.entry].toObject();
      settings["credential"] = "fixture-secret";
      configs[f.entry] = settings;
      f.configuration["configuration"] = configs;
    }
    QVERIFY(!f.inject("alice", "An observed message").isEmpty());
    auto accepted =
        ObservationStore(profile, f.path).ingest(f.binding(), f.envelope());
    QVERIFY(accepted);
    auto runtime = std::make_unique<PluginRuntime>(
        profile, f.directory.filePath("plugins.json"));
    runtime->setObservationDatabase(f.path);
    QVERIFY(runtime->setHostConfiguration(f.configuration));
    auto context = accepted.value().message.context;
    context.activityId = "send-activity";
    QVERIFY(runtime->loadCapability(f.ref(f.send), context, true));
    ToolExecutionRequest call;
    call.requestId = "actual-send";
    call.activityId = context.activityId;
    call.context = context;
    call.autonomous = true;
    call.toolName = "invoke_capability";
    call.arguments = {{"ref", f.ref(f.send).toJson()},
                      {"arguments", QJsonObject{{"chat_id", "group-a"},
                                                {"text", requested},
                                                {"reply_to", "message-1"}}}};
    std::optional<ToolExecutionOutcome> result;
    runtime->executeGateway(call, [&](auto value) { result = value; });
    QTRY_VERIFY(result);
    QVERIFY2(result->result.success, qPrintable(result->result.errorMessage));
    QCOMPARE(f.count("agent_response_dispatches", "status='completed'"), 1);
    QCOMPARE(f.count("agent_messages", "direction='outgoing'"), 1);
    auto history =
        ConversationStore(profile, f.path).loadHistory(context.conversationId);
    QVERIFY(history);
    QCOMPARE(history.value().last().content, actual);
    if (mode == "secret")
      QVERIFY(!QJsonDocument(result->result.data)
                   .toJson()
                   .contains("fixture-secret"));
    auto response = result->result.data;
    QVERIFY(!response["receipt"]
                 .toObject()["remoteMessageId"]
                 .toString()
                 .isEmpty());
    result.reset();
    runtime->executeGateway(call, [&](auto value) { result = value; });
    QTRY_VERIFY(result);
    QVERIFY(!result->result.success);
    QCOMPARE(f.count("agent_messages", "direction='outgoing'"), 1);
    auto echo = f.envelope("2", "group-a", "message-2", "pet-account");
    auto payload = echo.payload;
    auto msg = payload["message"].toObject();
    msg["text"] = actual;
    payload["message"] = msg;
    echo.payload = payload;
    auto self = ObservationStore(profile, f.path).ingest(f.binding(), echo);
    QVERIFY(self);
    QVERIFY(self.value().duplicate);
    QVERIFY(self.value().message.selfMessage);
    QCOMPARE(f.count("event_log", "type='MessageObserved'"), 2);
  }
  void lateReceiptStorageFailure_retainsActualFactAfterPluginDestruction() {
    Fixture f;
    QVERIFY(!f.inject("alice", "An observed message").isEmpty());
    auto accepted =
        ObservationStore(profile, f.path).ingest(f.binding(), f.envelope());
    QVERIFY(accepted);
    auto runtime = std::make_unique<PluginRuntime>(
        profile, f.directory.filePath("plugins.json"));
    runtime->setObservationDatabase(f.path);
    QVERIFY(runtime->setHostConfiguration(f.configuration));
    auto context = accepted.value().message.context;
    context.activityId = "late-activity";
    QVERIFY(runtime->loadCapability(f.ref(f.send), context, true));
    QVERIFY(f.sql(
        "CREATE TRIGGER blocked_receipt BEFORE INSERT ON agent_messages WHEN "
        "NEW.direction='outgoing' BEGIN SELECT RAISE(FAIL,'offline'); END"));
    ToolExecutionRequest call;
    call.requestId = "late-send";
    call.activityId = context.activityId;
    call.context = context;
    call.autonomous = true;
    call.toolName = "invoke_capability";
    call.arguments = {{"ref", f.ref(f.send).toJson()},
                      {"arguments", QJsonObject{{"chat_id", "group-a"},
                                                {"text", "Already sent"},
                                                {"reply_to", "message-1"}}}};
    std::optional<ToolExecutionOutcome> result;
    runtime->executeGateway(call, [&](auto value) { result = value; });
    QTRY_COMPARE(f.count("agent_response_dispatches", "status='dispatching'"),
                 1);
    QTest::qWait(300);
    QVERIFY(!result);
    runtime.reset();
    QVERIFY(f.sql("DROP TRIGGER blocked_receipt"));
    QTRY_VERIFY_WITH_TIMEOUT(result, 5000);
    QVERIFY(result->result.success);
    QCOMPARE(f.count("agent_messages", "direction='outgoing'"), 1);
  }
  void queryCompletesAfterRetire_archivesWithoutNewObservationNotification() {
    Fixture f;
    QVERIFY(!f.inject().isEmpty());
    QFile script(f.root + "/adapter.py");
    QVERIFY(script.open(QIODevice::ReadOnly));
    auto body = script.readAll();
    script.close();
    body.replace("elif args.operation == 'read':",
                 "elif args.operation == 'read':\n        time.sleep(.3)");
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(body);
    script.close();
    QPointer<PluginRuntime> runtime =
        new PluginRuntime(profile, f.directory.filePath("plugins.json"));
    runtime->setObservationDatabase(f.path);
    QVERIFY(runtime->setHostConfiguration(f.configuration));
    ContextSpec context;
    context.profileId = profile;
    QVERIFY(runtime->loadCapability(f.ref(f.read), context, true));
    QSignalSpy observations(runtime, &PluginRuntime::observationAccepted);
    QSignalSpy dispatched(runtime, &PluginRuntime::capabilityDispatching);
    ToolExecutionRequest call;
    call.requestId = "retired-query";
    call.activityId = "old-profile-check";
    call.context = context;
    call.autonomous = true;
    call.toolName = "invoke_capability";
    call.arguments = {{"ref", f.ref(f.read).toJson()},
                      {"arguments", QJsonObject{{"chat_id", "group-a"}}}};
    std::optional<ToolExecutionOutcome> result;
    runtime->executeGateway(call, [&](auto value) { result = value; });
    QTRY_COMPARE(dispatched.size(), 1);
    runtime->retire();
    QTRY_VERIFY(result);
    QVERIFY(result->result.success);
    QCOMPARE(f.count("agent_messages"), 1);
    QCOMPARE(observations.size(), 0);
    QTRY_VERIFY(runtime.isNull());
  }
  void failedPoll_thenSourceReplay_preservesBothFacts() {
    Fixture f;
    QVERIFY(!f.inject().isEmpty());
    QVERIFY(!f.inject("bob", "Second message").isEmpty());
    PluginRuntime runtime(profile, f.directory.filePath("plugins.json"));
    runtime.setObservationDatabase(f.path);
    QVERIFY(runtime.setHostConfiguration(f.configuration));
    ContextSpec context;
    context.profileId = profile;
    QVERIFY(runtime.loadCapability(f.ref(f.read), context, true));
    QVERIFY(f.sql("CREATE TRIGGER block_poll BEFORE INSERT ON agent_messages "
                  "BEGIN SELECT RAISE(FAIL,'offline'); END"));
    ToolExecutionRequest call;
    call.requestId = "failed-poll";
    call.activityId = "check";
    call.context = context;
    call.autonomous = true;
    call.toolName = "invoke_capability";
    call.arguments = {{"ref", f.ref(f.read).toJson()},
                      {"arguments", QJsonObject{{"chat_id", "group-a"}}}};
    std::optional<ToolExecutionOutcome> result;
    runtime.executeGateway(call, [&](auto value) { result = value; });
    QTRY_VERIFY(result);
    QVERIFY(!result->result.success);
    QCOMPARE(f.count("agent_messages"), 0);
    QVERIFY(f.sql("DROP TRIGGER block_poll"));
    runtime.setObservationSource(
        {"account", true, {}, runtime.catalogRevision()}, [](auto) {});
    QTRY_COMPARE(f.count("agent_messages"), 2);
    QCOMPARE(ObservationStore(profile, f.path).cursor(f.binding()).value(),
             QString("cursor:2"));
  }
  void processCrash_reconnectStartsAtCommittedCursor() {
    Fixture f;
    QVERIFY(f.sourceProbe(
        {f.envelope().payload, f.envelope("2", "group-a", "message-2").payload},
        "reconnect"));
    PluginRuntime runtime(profile, f.directory.filePath("plugins.json"));
    runtime.setObservationDatabase(f.path);
    QVERIFY(runtime.setHostConfiguration(f.configuration));
    QTRY_COMPARE_WITH_TIMEOUT(f.count("agent_messages"), 2, 5000);
    QTest::qWait(300);
    QFile log(f.directory.filePath("source-log.jsonl"));
    QVERIFY(log.open(QIODevice::ReadOnly));
    const auto lines = log.readAll();
    QVERIFY(lines.contains("cursor:1"));
    QCOMPARE(ObservationStore(profile, f.path).cursor(f.binding()).value(),
             QString("cursor:2"));
  }
  void preparedConversationReply_withoutNativeReplyMapping_sendsOnce() {
    Fixture f;
    QVERIFY(!f.inject("alice", "An observed message").isEmpty());
    auto accepted =
        ObservationStore(profile, f.path).ingest(f.binding(), f.envelope());
    QVERIFY(accepted);
    auto replies = f.configuration["replyBindings"].toArray();
    for (int i = 0; i < replies.size(); ++i) {
      auto b = replies[i].toObject();
      auto args = b["argumentBindings"].toObject();
      args.remove("/reply_to");
      b["argumentBindings"] = args;
      replies[i] = b;
    }
    f.configuration["replyBindings"] = replies;
    PluginRuntime runtime(profile, f.directory.filePath("plugins.json"));
    runtime.setObservationDatabase(f.path);
    QVERIFY(runtime.setHostConfiguration(f.configuration));
    ConversationService service(profile, f.path);
    for (const auto &b : runtime.conversationBindings())
      QVERIFY(service.registerBinding(b));
    ContextRequestV2 request;
    request.context = accepted.value().message.context;
    request.context.activityId = "prepared-send";
    request.requestId = "prepared-send";
    request.replyTarget = accepted.value().message.replyTarget;
    request.sourceMessageIds = {accepted.value().message.messageId};
    auto prepared = service.storeSnapshot().prepareContext(request);
    QVERIFY(prepared);
    RoutedResponse response{request.requestId,        0,
                            prepared.value().context, request.replyTarget,
                            "One service reply",      request.sourceMessageIds};
    std::optional<Result<void, DomainError>> result;
    service.publishResponseAsync(
        response, [&](auto value) { result = std::move(value); });
    QTRY_VERIFY(result);
    if (!*result)
      QFAIL(qPrintable(result->error().code + ": " + result->error().message));
    QCOMPARE(f.count("agent_messages", "direction='outgoing'"), 1);
    QCOMPARE(f.count("agent_response_dispatches", "status='completed'"), 1);
    result.reset();
    service.publishResponseAsync(
        response, [&](auto value) { result = std::move(value); });
    QTRY_VERIFY(result);
    QVERIFY(!*result);
    QCOMPARE(f.count("agent_messages", "direction='outgoing'"), 1);
  }
  void conversationArchive_storageFailureAndDestruction_retainsReceipt_data() {
    QTest::addColumn<QString>("table");
    QTest::newRow("message") << QString("agent_messages");
    QTest::newRow("outbox") << QString("event_outbox");
  }
  void conversationArchive_storageFailureAndDestruction_retainsReceipt() {
    QFETCH(QString, table);
    Fixture f;
    auto accepted =
        ObservationStore(profile, f.path).ingest(f.binding(), f.envelope());
    QVERIFY(accepted);
    auto service = std::make_unique<ConversationService>(profile, f.path);
    auto binding = f.binding().conversations.first();
    binding.toolName = "fixture-send";
    int sends = 0;
    binding.dispatcher = [&](const ReplyDispatch &dispatch,
                             ReplyCompletion done) {
      ++sends;
      QVERIFY(dispatch.dispatchToken->begin());
      DeliveryReceipt receipt;
      receipt.outcome = DeliveryOutcome::Completed;
      receipt.remoteMessageId = "actual-service-send";
      receipt.content = "Actual service receipt";
      receipt.occurredAt = QDateTime::currentDateTimeUtc();
      done(receipt);
    };
    QVERIFY(service->registerBinding(binding));
    ContextRequestV2 request;
    request.context = accepted.value().message.context;
    request.requestId = "service-archive";
    request.replyTarget = accepted.value().message.replyTarget;
    request.sourceMessageIds = {accepted.value().message.messageId};
    auto context = service->storeSnapshot().prepareContext(request);
    QVERIFY(context);
    // A message-table failure is independent of event encoding and rolls the
    // full receipt back.
    QVERIFY(f.sql(
        "CREATE TRIGGER reject_message BEFORE INSERT ON " + table +
        (table == "agent_messages" ? " WHEN NEW.direction='outgoing'" : "") +
        " BEGIN SELECT RAISE(FAIL,'offline'); END"));
    std::optional<Result<void, DomainError>> result;
    service->publishResponseAsync({request.requestId, 0,
                                   context.value().context, request.replyTarget,
                                   "Requested reply", request.sourceMessageIds},
                                  [&](auto r) { result = std::move(r); });
    QTRY_VERIFY(result);
    QVERIFY(!*result);
    QCOMPARE(sends, 1);
    service.reset();
    QVERIFY(f.sql("DROP TRIGGER reject_message"));
    QTRY_COMPARE(f.count("agent_messages", "direction='outgoing'"), 1);
    QCOMPARE(sends, 1);
  }
  void corruptedCursorStorage_failsClosed_data() {
    QTest::addColumn<QString>("value");
    for (const auto &value : {"[]", "{\"broken\":true}",
                              "{\"sequence\":\"01\",\"cursor\":\"cursor:1\"}",
                              "{\"sequence\":\"1\",\"cursor\":4}"})
      QTest::newRow(value) << QString(value);
  }
  void corruptedCursorStorage_failsClosed() {
    QFETCH(QString, value);
    Fixture f;
    ObservationStore store(profile, f.path);
    QVERIFY(store.ingest(f.binding(), f.envelope()));
    {
      auto u = SqliteRuntimeUnitOfWork::begin(f.path);
      QVERIFY(u);
      auto db = QSqlDatabase::database(u.value()->connectionName());
      QSqlQuery q(db);
      QVERIFY(q.exec("SELECT control_json FROM agent_runtime_state"));
      QVERIFY(q.next());
      auto control = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
      auto cursors = control["sourceCursors"].toObject();
      const auto parsed = QJsonDocument::fromJson(value.toUtf8());
      if (parsed.isArray())
        control["sourceCursors"] = parsed.array();
      else {
        cursors[cursors.keys().first()] = parsed.object();
        control["sourceCursors"] = cursors;
      }
      q.prepare("UPDATE agent_runtime_state SET control_json=?");
      q.addBindValue(QString::fromUtf8(
          QJsonDocument(control).toJson(QJsonDocument::Compact)));
      QVERIFY(q.exec());
      QVERIFY(u.value()->commit());
    }
    auto cursor = store.cursor(f.binding());
    QVERIFY(!cursor);
    QCOMPARE(cursor.error().code, QString("STORE_UNAVAILABLE"));
    QVERIFY(
        !store.ingest(f.binding(), f.envelope("2", "group-a", "message-2")));
    QCOMPARE(f.count("agent_messages"), 1);
  }
  void partialConversationQuery_doesNotSkipOtherConversationWhenSourceStarts() {
    Fixture f;
    QVERIFY(!f.inject("bob", "First journal event", "group-b").isEmpty());
    QVERIFY(!f.inject("alice", "Second journal event", "group-a").isEmpty());
    PluginRuntime runtime(profile, f.directory.filePath("plugins.json"));
    runtime.setObservationDatabase(f.path);
    QVERIFY(runtime.setHostConfiguration(f.configuration));
    ContextSpec context;
    context.profileId = profile;
    QVERIFY(runtime.loadCapability(f.ref(f.read), context, true));
    ToolExecutionRequest call;
    call.requestId = "partial-read";
    call.activityId = "check";
    call.context = context;
    call.autonomous = true;
    call.toolName = "invoke_capability";
    call.arguments = {{"ref", f.ref(f.read).toJson()},
                      {"arguments", QJsonObject{{"chat_id", "group-a"}}}};
    std::optional<ToolExecutionOutcome> result;
    runtime.executeGateway(call, [&](auto value) { result = value; });
    QTRY_VERIFY(result);
    QVERIFY(result->result.success);
    QCOMPARE(f.count("agent_messages"), 1);
    QCOMPARE(ObservationStore(profile, f.path).cursor(f.binding()).value(),
             QString());
    runtime.setObservationSource(
        {"account", true, {}, runtime.catalogRevision()}, [](auto) {});
    QTRY_COMPARE(f.count("agent_messages"), 2);
    QCOMPARE(ObservationStore(profile, f.path).cursor(f.binding()).value(),
             QString("cursor:2"));
    runtime.setObservationSource(
        {"account", false, {}, runtime.catalogRevision()}, [](auto) {});
  }
  void missingTimeOrSpeaker_preservesUnknownIdentityAndTime() {
    QTemporaryDir dir;
    ConversationStore store("5bb00e6d-937a-4f46-9c87-e3933c078f5a",
                            dir.filePath("runtime.sqlite"));
    QVERIFY(store.initialize());
    HostConversationBinding binding;
    binding.channel = "controlled";
    binding.connectionKey = "one";
    binding.remoteConversationId = "group";
    binding.kind = ConversationKind::Group;
    QVERIFY(store.registerBinding(binding));
    IncomingMessage msg;
    msg.channel = binding.channel;
    msg.connectionKey = binding.connectionKey;
    msg.remoteConversationId = binding.remoteConversationId;
    msg.kind = binding.kind;
    msg.remoteMessageId = "one";
    msg.content = "A real message without an original timestamp";
    auto accepted = store.acceptMessage(msg);
    QVERIFY2(accepted,
             qPrintable(accepted ? QString() : accepted.error().message));
    QVERIFY(!accepted.value().personResolution.personId);
    auto history = store.loadHistory(accepted.value().conversationId);
    QVERIFY(history);
    QVERIFY(!history.value().first().timestamp.isValid());
  }
  void sourceContract_validDefinition_isAccepted() {
    QJsonObject source{
        {"formatVersion", "1"},
        {"id", "example.events"},
        {"protocol", "agent-observation-1"},
        {"requirements", QJsonArray{}},
        {"configuration", QJsonArray{QJsonObject{{"key", "program"},
                                                 {"kind", "executable"},
                                                 {"description", "Python"},
                                                 {"required", true}}}},
        {"invocation",
         QJsonObject{{"kind", "command"},
                     {"program", QJsonObject{{"setting", "program"}}},
                     {"arguments", QJsonArray{"--events"}}}}};
    auto valid = SchemaValidator::validateManifest("source", source);
    QVERIFY2(valid, qPrintable(valid ? QString() : valid.error().message));
  }
};
QTEST_GUILESS_MAIN(ExternalObservationTests)
#include "test_external_observation.moc"
