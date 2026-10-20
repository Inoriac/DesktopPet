#include "ai/plugin/plugin_runtime.h"
#include "ai/tools/runtime/tool_result_sanitizer.h"
#include "ai/event/runtime_unit_of_work.h"
#include "ai/observation/observation_store.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>

namespace {
const QString pluginId = "desktop-pet.qq", toolId = pluginId + ".tools", skillId = pluginId + ".skill";
const QString profileId = "12345678-1234-4234-8234-123456789abc";
QJsonObject readJson(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}
bool writeJson(const QString &path, const QJsonObject &object) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(object).toJson()) > 0;
}
QJsonObject message(QString room = "group:123", QString id = "m1") {
    const bool group = room.startsWith("group:");
    const QJsonObject text{{"type", "text"}, {"data", QJsonObject{{"text", QString::fromUtf8("你好🙂")}}}};
    return {{"post_type", "message"}, {"message_type", group ? "group" : "private"},
            {"message_id", id}, {"self_id", 999}, {"user_id", 456}, {"time", 1770000000},
            {group ? "group_id" : "target_id", room.section(':', 1).toInt()},
            {"sender", QJsonObject{{"user_id", 456}, {"nickname", QString::fromUtf8("朋友")}}},
            {"message", QJsonArray{text}}};
}
struct Fixture {
    QTemporaryDir directory{QDir::tempPath() + "/qq plugin 中文 XXXXXX"};
    QProcess backend;
    QString error;
    QString profilePath() const { return directory.filePath(profileId); }
    QString hostPath() const { return profilePath() + "/plugins.json"; }
    QString databasePath() const { return directory.filePath("runtime.sqlite"); }
    QString settingsPath() const { return directory.filePath("settings.json"); }
    ~Fixture() { backend.kill(); backend.waitForFinished(3000); }
    bool prepare(QJsonObject scenario = {}, QString autonomy = "on", QString source = "off") {
        QDir().mkpath(profilePath());
        if (!writeJson(directory.filePath("scenario.json"), scenario)) return false;
        backend.start(QString(QQ_TEST_PYTHON), {"-B", QString(QQ_BACKEND), "--ready-file", directory.filePath("ready.json"),
                      "--scenario", directory.filePath("scenario.json"), "--action-log", directory.filePath("actions.jsonl")});
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000 && !readJson(directory.filePath("ready.json")).contains("wsUrl")) QTest::qWait(20);
        const QJsonValue address = readJson(directory.filePath("ready.json")).value("wsUrl");
        if (!address.isString()) { error = QString::fromUtf8(backend.readAllStandardError()); return false; }
        if (!writeJson(settingsPath(), {{"backend", "napcat"}, {"wsUrl", address}, {"selfId", "999"},
                {"connectionKey", "test-account"}, {"stateDir", directory.filePath("state")},
                {"allowedConversations", QJsonArray{"group:123", "private:456"}}, {"requestTimeoutSeconds", 2}})) return false;
        ConversationStore store(profileId, databasePath());
        if (!store.initialize()) return false;
        return configure(autonomy, source);
    }
    bool configure(QString autonomy = "on", QString source = "off") {
        QProcess process;
        process.start(QString(QQ_TEST_PYTHON), {"-B", QString(QQ_PACKAGE) + "/configure.py", "--profile-dir", profilePath(),
                      "--settings", settingsPath(), "--autonomous", autonomy, "--source", source, "--apply"});
        if (!process.waitForFinished(5000) || process.exitCode() != 0) {
            error = QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError()); return false;
        }
        return true;
    }
    int count(QString table, QString condition = "1") const {
        auto unit = SqliteRuntimeUnitOfWork::begin(databasePath());
        if (!unit) return -1;
        QSqlQuery query(QSqlDatabase::database(unit.value()->connectionName()));
        return query.exec("SELECT count(*) FROM " + table + " WHERE " + condition) && query.next() ? query.value(0).toInt() : -1;
    }
    int actions(QString action = {}) const {
        QFile file(directory.filePath("actions.jsonl"));
        if (!file.open(QIODevice::ReadOnly)) return 0;
        int count = 0;
        for (const auto &line : file.readAll().split('\n')) {
            const auto value = QJsonDocument::fromJson(line).object();
            if (!value.isEmpty() && (action.isEmpty() || value["action"] == action)) ++count;
        }
        return count;
    }
    ContextSpec external(QString chat) const {
        auto context = ConversationStore(profileId, databasePath()).currentContext(conversationIdFor(profileId, "qq", "test-account", chat));
        return context ? context.value() : ContextSpec{};
    }
    ObservationBinding binding() const {
        auto config = ObservationConfiguration::parse(readJson(hostPath()));
        return config ? config.value().bindings[pluginId + ".account"] : ObservationBinding{};
    }
};
CapabilityRef ref(QString operation = {}) { return {pluginId, toolId, "1.0.0", operation}; }
ToolExecutionOutcome gateway(PluginRuntime &runtime, const ContextSpec &context, QString name, QJsonObject args = {}, bool autonomous = false) {
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    auto output = std::make_shared<std::optional<ToolExecutionOutcome>>();
    ToolExecutionRequest request;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.activityId = "qq-integration";
    request.context = context;
    request.autonomous = autonomous;
    request.toolName = name;
    request.arguments = args;
    request.timeoutMs = 5000;
    QPointer<QEventLoop> guard(&loop);
    runtime.executeGateway(request, [guard, output](ToolExecutionOutcome value) {
        if (guard) QMetaObject::invokeMethod(guard, [guard, output, value = std::move(value)]() mutable {
            *output = std::move(value);
            if (guard) guard->quit();
        }, Qt::QueuedConnection);
    });
    deadline.start(10000);
    loop.exec();
    if (*output) return **output;
    ToolExecutionOutcome failed;
    failed.result = ToolResult::fail("QQ gateway timed out");
    return failed;
}
ToolExecutionOutcome invoke(PluginRuntime &runtime, const ContextSpec &context, QString operation, QJsonObject args = {}, bool autonomous = false) {
    auto loaded = gateway(runtime, context, "load_capability", {{"ref", ref(operation).toJson()}}, autonomous);
    if (!loaded.result.success) return loaded;
    return gateway(runtime, context, "invoke_capability", {{"ref", ref(operation).toJson()}, {"arguments", args}}, autonomous);
}
}

class QQPluginTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QProcess check;
        check.start(QString(QQ_TEST_PYTHON), {"-B", "-c", "import websockets.sync.client, websockets.sync.server"});
        if (!check.waitForFinished(5000) || check.exitCode() != 0)
            QSKIP("QQ_TEST_DEPENDENCY_MISSING: select a Python environment with plugins/qq/requirements.txt using QQ_TEST_PYTHON");
    }
    void runtime_load_read_archive_send_receipt_data() {
        QTest::addColumn<QString>("chat");
        QTest::newRow("group") << QString("group:123");
        QTest::newRow("private") << QString("private:456");
    }
    void runtime_load_read_archive_send_receipt() {
        QFETCH(QString, chat);
        Fixture f;
        QVERIFY2(f.prepare({{"history", QJsonObject{{chat, QJsonArray{message(chat)}}}}}), qPrintable(f.error));
        PluginRuntime runtime(profileId, f.hostPath());
        runtime.setObservationDatabase(f.databasePath());
        auto initialized = runtime.initialize();
        QVERIFY2(initialized, qPrintable(initialized ? QString() : initialized.error().message));
        const auto local = ContextSpec::localOwner(profileId);
        auto discovered = gateway(runtime, local, "discover_capabilities");
        QVERIFY(discovered.result.success);
        QCOMPARE(discovered.result.data["entries"].toArray().size(), 2);
        QVERIFY(!QJsonDocument(discovered.result.data).toJson().contains("instructions"));
        CapabilityRef skill{pluginId, skillId, "1.0.0", {}};
        auto loaded = gateway(runtime, local, "load_capability", {{"ref", skill.toJson()}});
        QVERIFY2(loaded.result.success, qPrintable(loaded.result.errorMessage));
        QVERIFY(loaded.result.data["instructions"].toString().contains(QString::fromUtf8("等待")));
        QCOMPARE(f.actions(), 0);
        auto read = invoke(runtime, local, "read", {{"chat_id", chat}});
        QVERIFY2(read.result.success, qPrintable(read.result.errorMessage));
        QCOMPARE(read.result.data["data"].toObject()["items"].toArray().size(), 1);
        QCOMPARE(f.count("agent_messages"), 1);
        QCOMPARE(ObservationStore(profileId, f.databasePath()).cursor(f.binding()).value(), QString()); // queries never ACK listener
        QVERIFY(invoke(runtime, local, "read", {{"chat_id", chat}}).result.success);
        QCOMPARE(f.count("agent_messages"), 1);
        auto context = f.external(chat);
        QCOMPARE(context.originKind, OriginKind::External);
        auto sent = invoke(runtime, context, "send", {{"chat_id", chat}, {"text", QString::fromUtf8("收到🙂")}, {"reply_to", "m1"}}, true);
        QVERIFY2(sent.result.success, qPrintable(sent.result.errorMessage));
        QCOMPARE(sent.result.data["stage"].toString(), "completed");
        QCOMPARE(f.count("agent_response_dispatches", "status='completed'"), 1);
        QCOMPARE(f.count("agent_messages", "direction='outgoing'"), 1);
        auto history = ConversationStore(profileId, f.databasePath()).loadHistory(context.conversationId);
        QVERIFY(history);
        QCOMPARE(history.value().last().content, QString::fromUtf8("收到🙂"));
        const auto receiptId = sent.result.data["data"].toObject()["receipt_id"].toString();
        QVERIFY(!receiptId.isEmpty());
        auto receipt = invoke(runtime, f.external(chat), "receipt", {{"chat_id", chat}, {"receipt_id", receiptId}}, true);
        QVERIFY2(receipt.result.success, qPrintable(receipt.result.errorMessage));
        QCOMPARE(receipt.result.data["data"].toObject()["delivery"].toString(), "completed");
        QCOMPARE(f.actions(chat.startsWith("group:") ? "send_group_msg" : "send_private_msg"), 1);
        QVERIFY(!QFile::exists(QString(QQ_PACKAGE) + "/__pycache__"));
    }
    void runtime_scope_and_disable() {
        Fixture f;
        QVERIFY2(f.prepare({{"history", QJsonObject{{"group:123", QJsonArray{message()}}}}}), qPrintable(f.error));
        PluginRuntime runtime(profileId, f.hostPath());
        runtime.setObservationDatabase(f.databasePath());
        QVERIFY(runtime.initialize());
        const auto local = ContextSpec::localOwner(profileId);
        QVERIFY(invoke(runtime, local, "read", {{"chat_id", "group:123"}}).result.success);
        auto external = f.external("group:123");
        const int before = f.actions();
        QVERIFY(!invoke(runtime, local, "send", {{"chat_id", "group:123"}, {"text", "denied"}, {"reply_to", ""}}).result.success);
        QVERIFY(!invoke(runtime, external, "list", {}, true).result.success);
        for (const auto &operation : {"read", "send", "receipt"}) {
            QJsonObject arguments{{"chat_id", "private:456"}};
            if (QString(operation) == "send") { arguments["text"] = "denied"; arguments["reply_to"] = ""; }
            if (QString(operation) == "receipt") arguments["receipt_id"] = "not-owned";
            QVERIFY(!invoke(runtime, external, operation, arguments, true).result.success);
        }
        QCOMPARE(f.actions(), before);
        QVERIFY(runtime.loadCapability(ref("read"), local, false));
        QVERIFY(runtime.setPluginEnabled({pluginId, false, runtime.catalogRevision()}));
        auto disabled = gateway(runtime, local, "invoke_capability", {{"ref", ref("read").toJson()}, {"arguments", QJsonObject{{"chat_id", "group:123"}}}});
        QVERIFY(!disabled.result.success);
        QVERIFY(!disabled.executed);
        QCOMPARE(f.actions(), before);
    }
    void unknown_delivery_retains_receipt_without_resending() {
        Fixture f;
        QVERIFY2(f.prepare({{"history", QJsonObject{{"group:123", QJsonArray{message()}}}},
                            {"delivery", QJsonObject{{"omit_message_id", true}}}}), qPrintable(f.error));
        PluginRuntime runtime(profileId, f.hostPath());
        runtime.setObservationDatabase(f.databasePath());
        QVERIFY(runtime.initialize());
        QVERIFY(invoke(runtime, ContextSpec::localOwner(profileId), "read", {{"chat_id", "group:123"}}).result.success);
        auto sent = invoke(runtime, f.external("group:123"), "send",
                           {{"chat_id", "group:123"}, {"text", "Possibly delivered"}, {"reply_to", ""}}, true);
        QVERIFY(!sent.result.success);
        QCOMPARE(sent.result.data["stage"].toString(), "unknown");
        QCOMPARE(f.count("agent_response_dispatches", "status='completed'"), 0);
        QCOMPARE(f.count("agent_response_dispatches", "status='unknown'"), 1);
        const auto receiptId = sent.result.data["data"].toObject()["receipt_id"].toString();
        QVERIFY(!receiptId.isEmpty());
        auto receipt = invoke(runtime, f.external("group:123"), "receipt",
                              {{"chat_id", "group:123"}, {"receipt_id", receiptId}}, true);
        QVERIFY2(receipt.result.success, qPrintable(receipt.result.errorMessage));
        QCOMPARE(receipt.result.data["data"].toObject()["delivery"].toString(), "unknown");
        QCOMPARE(f.actions("send_group_msg"), 1);
        QCOMPARE(f.count("agent_messages", "direction='outgoing'"), 0);
    }
    void long_read_pages_fit_model_payload_and_archive_originals() {
        Fixture f;
        QJsonArray history;
        const QString original = QString::fromUtf8("阅读🙂").repeated(700);
        for (int i = 1; i <= 8; ++i) {
            auto item = message("group:123", QString::number(i));
            item["message"] = QJsonArray{QJsonObject{{"type", "text"}, {"data", QJsonObject{{"text", original}}}}};
            history.append(item);
        }
        QVERIFY2(f.prepare({{"history", QJsonObject{{"group:123", history}}}}), qPrintable(f.error));
        PluginRuntime runtime(profileId, f.hostPath());
        runtime.setObservationDatabase(f.databasePath());
        QVERIFY(runtime.initialize());
        QString cursor;
        int pages = 0, messages = 0;
        do {
            auto page = invoke(runtime, ContextSpec::localOwner(profileId), "read", {{"chat_id", "group:123"}, {"cursor", cursor}, {"limit", 20}});
            QVERIFY2(page.result.success, qPrintable(page.result.errorMessage));
            const auto payload = ToolResultSanitizer().toPayload(page.result);
            QVERIFY(payload.size() <= 6000);
            QVERIFY(!payload.contains("Tool result omitted"));
            const auto data = page.result.data["data"].toObject();
            messages += data["items"].toArray().size();
            cursor = data["next_cursor"].toString();
            QVERIFY(++pages <= 8);
        } while (!cursor.isEmpty());
        QVERIFY(pages > 1);
        QCOMPARE(messages, 8);
        const auto archived = ConversationStore(profileId, f.databasePath()).loadHistory(f.external("group:123").conversationId);
        QVERIFY(archived);
        QCOMPARE(archived.value().size(), 8);
        for (const auto &entry : archived.value()) QCOMPARE(entry.content, original);
    }
    void source_autonomy_independent_and_replay_deduplicates() {
        Fixture f;
        QVERIFY2(f.prepare({{"events", QJsonArray{message()}}}, "off", "on"), qPrintable(f.error));
        PluginRuntime runtime(profileId, f.hostPath());
        runtime.setObservationDatabase(f.databasePath());
        auto initialized = runtime.initialize();
        QVERIFY2(initialized, qPrintable(initialized ? QString() : initialized.error().message));
        ContextSpec internal;
        internal.profileId = profileId;
        internal.activityId = "idle";
        QVERIFY(runtime.discoverCapabilities(internal, true).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(f.count("agent_messages"), 1, 5000);
        auto store = ObservationStore(profileId, f.databasePath());
        QTRY_VERIFY_WITH_TIMEOUT(!store.cursor(f.binding()).value().isEmpty(), 5000);
        const auto cursor = store.cursor(f.binding()).value();
        std::optional<Result<SourceState, DomainError>> control;
        runtime.setObservationSource({pluginId + ".account", false, {}, runtime.catalogRevision()}, [&](auto value) { control = value; });
        QVERIFY(control && *control);
        runtime.setObservationSource({pluginId + ".account", true, {}, runtime.catalogRevision()}, [&](auto value) { control = value; });
        QVERIFY(control && *control);
        QTRY_VERIFY_WITH_TIMEOUT(f.actions("get_login_info") >= 2, 5000);
        QTest::qWait(150);
        QCOMPARE(f.count("agent_messages"), 1);
        QCOMPARE(store.cursor(f.binding()).value(), cursor);
        runtime.setObservationSource({pluginId + ".account", false, {}, runtime.catalogRevision()}, [](auto) {});
    }
};

QTEST_GUILESS_MAIN(QQPluginTests)
#include "test_qq_plugin.moc"
