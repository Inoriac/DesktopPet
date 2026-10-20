#include "ai/plugin/plugin_catalog.h"
#include "ai/plugin/plugin_runtime.h"
#include "ai/plugin/capability_context.h"
#include "ai/plugin/schema_validator.h"
#include "ai/tools/runtime/tool_result_sanitizer.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QPointer>
#include <QTemporaryDir>
#include <QtTest>

namespace {
struct CommandOutput { QJsonObject envelope; QString error; };
CommandOutput execute(const QString& program, const QStringList& arguments, const QByteArray& input = "{}") {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(5000)) return {{}, process.errorString()};
    process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(10000)) {
        process.kill(); process.waitForFinished();
        return {{}, "Command timed out"};
    }
    const auto stdoutBytes = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {{}, QString("Unexpected exit %1 (status %2): ").arg(process.exitCode()).arg(process.exitStatus())
                    + QString::fromUtf8(process.readAllStandardError())};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(stdoutBytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {{}, "stdout was not one JSON object: " + QString::fromUtf8(stdoutBytes)};
    return {document.object(), {}};
}
struct Fixture {
    QTemporaryDir directory{QDir::tempPath() + "/web command 中文 XXXXXX"};
    QString stateDir() const { return directory.filePath("state with 空格"); }
    CommandOutput invoke(QString operation, QJsonObject arguments = {}, bool production = false) const {
        return execute(production ? QString(WEB_COMMAND_PROGRAM) : QString(WEB_COMMAND_FIXTURE),
                       {"--state-dir", stateDir(), "--operation", operation},
                       QJsonDocument(arguments).toJson(QJsonDocument::Compact));
    }
    int calls() const {
        QFile file(QDir(stateDir()).filePath("backend-calls.jsonl"));
        return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
    }
};
QJsonObject readJson(const QString& name) {
    QFile file(QString(WEB_PACKAGE) + '/' + name);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
}

class WebCommandTests : public QObject {
    Q_OBJECT
private slots:
    void gateway_readFailureAndPermissionBoundaries() {
        Fixture fixture;
        const QString profile = "12345678-1234-4234-8234-123456789abc";
        const QString plugin = "desktop-pet.web", entry = plugin + ".tools";
        const CapabilityRef ref{plugin, entry, "1.0.0", "read_page"};
        const auto context = ContextSpec::localOwner(profile);
        PluginRuntime runtime(profile, fixture.directory.filePath("plugins.json"));
        QJsonObject configuration{
            {"plugins", QJsonArray{QJsonObject{{"pluginId", plugin}, {"rootPath", QString(WEB_PACKAGE)}, {"enabled", true}}}},
            {"configuration", QJsonObject{{entry, QJsonObject{{"program", QString(WEB_COMMAND_FIXTURE)}, {"state_dir", fixture.stateDir()}}}}},
            {"grants", QJsonArray{QJsonObject{{"id", "web.local.read"}, {"profileId", profile}, {"pluginId", plugin},
                {"entryId", entry}, {"operationId", "read_page"}, {"conversationId", "local_owner"},
                {"effects", QJsonArray{"read"}}, {"executor", QString(WEB_COMMAND_FIXTURE)}, {"autonomous", false}}}},
            {"resultAdapters", QJsonObject{{entry + "/read_page", QJsonObject{{"dataPointer", "/data"}, {"statePointer", "/state"},
                {"stateMap", QJsonObject{{"completed", "completed"}, {"failed", "failed"}}}}}}}};
        QVERIFY(runtime.setHostConfiguration(configuration));
        auto gateway = [&](QString name, QJsonObject arguments, ContextSpec ctx,
                           QJsonObject definitionBinding = QJsonObject{}) {
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            auto output = std::make_shared<std::optional<ToolExecutionOutcome>>();
            ToolExecutionRequest request;
            request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            request.activityId = "web-check"; request.context = ctx;
            request.toolName = name; request.arguments = arguments; request.timeoutMs = 5000;
            request.capabilityDefinition = definitionBinding;
            QPointer<QEventLoop> guard(&loop);
            runtime.executeGateway(request, [output, guard](ToolExecutionOutcome result) {
                if (guard) QMetaObject::invokeMethod(guard, [output, guard, result = std::move(result)]() mutable {
                    *output = std::move(result); if (guard) guard->quit();
                }, Qt::QueuedConnection);
            });
            timer.start(10000); loop.exec();
            ToolExecutionOutcome failed; failed.result = ToolResult::fail("Gateway timed out");
            return *output ? **output : failed;
        };
        QVERIFY(!gateway("invoke_capability", {{"ref", ref.toJson()},
            {"arguments", QJsonObject{{"url", "https://example.com/code"}}}}, context).result.success);
        const auto loaded = gateway("load_capability", {{"ref", ref.toJson()}}, context);
        QVERIFY(loaded.result.success);
        const auto projection = CapabilityContext::projectTools(runtime.gatewaySchemas(context),
            CapabilityContext::projectSteps({QJsonObject{{"name", "load_capability"},
                {"status", "succeeded"}, {"output", loaded.result.data}}}));
        const auto alias = projection.operationRefs.key(ref.toJson());
        QVERIFY(!alias.isEmpty());
        auto invoke = [&](QJsonObject args, ContextSpec ctx) {
            LlmToolCall call{"model-call", "function", alias, args};
            QJsonObject definitionBinding;
            if (!projection.normalizeCall(call, &definitionBinding)) {
                ToolExecutionOutcome failed;
                failed.result = ToolResult::fail("Alias unavailable");
                return failed;
            }
            return gateway(call.name, call.arguments, ctx, definitionBinding);
        };
        QCOMPARE(fixture.calls(), 0);
        const auto invalid = invoke({}, context);
        QVERIFY(!invalid.result.success);
        QVERIFY(!invalid.result.data["executed"].toBool());
        QCOMPARE(fixture.calls(), 0);
        const auto page = invoke({{"url", "https://example.com/code"}}, context);
        QVERIFY2(page.result.success, qPrintable(page.result.errorMessage));
        QVERIFY(page.result.data["data"].toObject()["content"].toString().contains("print('hello')"));
        const auto failure = invoke({{"url", "https://example.com/network-failure"}}, context);
        QVERIFY(!failure.result.success);
        ToolResultSanitizer sanitizer;
        const auto payload = QJsonDocument::fromJson(sanitizer.toPayload(failure.result).toUtf8()).object();
        QCOMPARE(payload["data"].toObject()["data"].toObject()["error"].toObject()["code"].toString(), "NETWORK_ERROR");
        ContextSpec external = context; external.originKind = OriginKind::External; external.conversationId = "external";
        QVERIFY(!gateway("load_capability", {{"ref", ref.toJson()}}, external).result.success);
        QVERIFY(!invoke({{"url", "https://example.com/code"}}, external).result.success);
        QVERIFY(runtime.setPluginEnabled({plugin, false, runtime.catalogRevision()}));
        QVERIFY(!invoke({{"url", "https://example.com/code"}}, context).result.success);
        QVERIFY(runtime.setPluginEnabled({plugin, true, runtime.catalogRevision()}));
        const auto stale = invoke({{"url", "https://example.com/code"}}, context);
        QVERIFY(!stale.result.success);
        QCOMPARE(stale.result.errorMessage, "STALE_REVISION");
        const auto reloaded = gateway("load_capability", {{"ref", ref.toJson()}}, context);
        QVERIFY(reloaded.result.success);
        QVERIFY(reloaded.result.data["catalogRevision"] != loaded.result.data["catalogRevision"]);
        // A fresh host cache must not upgrade an already-issued model request.
        QCOMPARE(invoke({{"url", "https://example.com/code"}}, context).result.errorMessage, "STALE_REVISION");
        QCOMPARE(fixture.calls(), 2);
    }

    void command_status_localSuccess() {
        Fixture fixture;
        for (bool production : {false, true}) {
            const auto result = fixture.invoke("status", {}, production);
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QCOMPARE(result.envelope["state"].toString(), "completed");
            const auto data = result.envelope["data"].toObject();
            QCOMPARE(data["provider"].toString(), "bing");
            QVERIFY(data["cacheDirConfigured"].toBool());
            QCOMPARE(data["operations"].toArray(), (QJsonArray{"status", "search", "read_page"}));
            QVERIFY(!data["limitations"].toArray().isEmpty());
        }
        QCOMPARE(fixture.calls(), 0);
        QVERIFY(!QDir(fixture.stateDir()).exists());
    }

    void command_searchAndRead_realProcessPagination() {
        Fixture fixture;
        const auto search = fixture.invoke("search", {{"query", "Qt 中文"}, {"limit", 2}});
        QVERIFY2(search.error.isEmpty(), qPrintable(search.error));
        QCOMPARE(search.envelope["state"].toString(), "completed");
        const auto entries = search.envelope["data"].toObject()["entries"].toArray();
        QCOMPARE(entries.size(), 2);
        const QString url = entries[0].toObject()["url"].toString();
        QCOMPARE(url, "https://example.com/article");
        QString cursor, text;
        int pages = 0;
        do {
            QJsonObject input{{"url", url}};
            if (!cursor.isEmpty()) input["cursor"] = cursor;
            const auto page = fixture.invoke("read_page", input);
            QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
            QCOMPARE(page.envelope["state"].toString(), "completed");
            const auto data = page.envelope["data"].toObject();
            QCOMPARE(data["url"].toString(), url);
            text += data["content"].toString();
            cursor = data["nextCursor"].toString();
            QCOMPARE(data["hasMore"].toBool(), !cursor.isEmpty());
            QVERIFY(++pages < 30);
        } while (!cursor.isEmpty());
        QVERIFY(pages > 1);
        QCOMPARE(text, QString::fromUtf8("网页🙂 \"\\\n").repeated(900));
        QCOMPARE(fixture.calls(), 2);
    }

    void command_invalidInputAndFailure_failedEnvelope() {
        Fixture fixture;
        const QStringList base{"--state-dir", fixture.stateDir(), "--operation", "search"};
        const QList<QByteArray> invalidInput{"", "{broken", "[]", "{} {}", "{\"query\":true}", "{\"query\":\"Qt\",\"limit\":true}",
            "{\"query\":\"Qt\",\"limit\":1.5}", "{\"query\":\"Qt\",\"limit\":6}", "{\"query\":\"Qt\",\"extra\":1}",
            "{\"query\":\"Qt\",\"state_dir\":\"/tmp/override\"}", QByteArray(16 * 1024 + 1, ' ')};
        for (const auto& input : invalidInput) {
            const auto result = execute(WEB_COMMAND_FIXTURE, base, input);
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QCOMPARE(result.envelope["state"].toString(), "failed");
            QCOMPARE(result.envelope["data"].toObject()["error"].toObject()["code"].toString(), "INVALID_ARGUMENT");
        }
        for (const QStringList& arguments : QList<QStringList>{{}, {"--operation", "status"},
             {"--state-dir", "relative", "--operation", "status"},
             {"--state-dir", fixture.stateDir(), "--operation", "unknown"},
             {"--state-dir", fixture.stateDir(), "--operation", "status", "--unknown"},
             {"--state-dir", fixture.stateDir(), "--operation", "status", "--operation", "search"}}) {
            const auto result = execute(WEB_COMMAND_PROGRAM, arguments);
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QCOMPARE(result.envelope["state"].toString(), "failed");
            QCOMPARE(result.envelope["data"].toObject()["error"].toObject()["code"].toString(), "INVALID_ARGUMENT");
        }
        for (const auto& input : {QJsonObject{{"url", "https://example.com/article"}, {"cursor", 1}},
                                 QJsonObject{{"url", "https://example.com/article"}, {"cursor", ""}},
                                 QJsonObject{{"url", "https://example.com/article"}, {"timeout_ms", 2000}}}) {
            const auto result = fixture.invoke("read_page", input);
            QCOMPARE(result.envelope["state"].toString(), "failed");
        }
        QCOMPARE(fixture.calls(), 0);
        QCOMPARE(fixture.invoke("read_page", {{"url", "http://127.0.0.1/"}}).envelope["data"].toObject()["error"].toObject()["code"].toString(), "URL_NOT_ALLOWED");
        const auto failed = fixture.invoke("search", {{"query", "network-failure"}});
        QCOMPARE(failed.envelope["state"].toString(), "failed");
        QCOMPARE(failed.envelope["data"].toObject()["error"].toObject()["code"].toString(), "NETWORK_ERROR");
        QVERIFY(!failed.envelope["data"].toObject().contains("entries"));
        QCOMPARE(fixture.calls(), 1);
    }

    void command_sanitizer_preservesBudgetAndCursors() {
        Fixture fixture;
        auto search = fixture.invoke("search", {{"query", "long-results"}});
        auto page = fixture.invoke("read_page", {{"url", "https://example.com/article"}});
        for (const auto& envelope : {search.envelope, page.envelope}) {
            QCOMPARE(envelope["state"].toString(), "completed");
            const auto data = envelope["data"].toObject();
            QVERIFY(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)).size() <= 3800);
            ToolResultSanitizer sanitizer;
            const auto payload = sanitizer.toPayload(ToolResult::ok({{"stage", "completed"}, {"data", data}}));
            QVERIFY(payload.size() <= 6000);
            QCOMPARE(QJsonDocument::fromJson(payload.toUtf8()).object()["data"].toObject()["data"].toObject(), data);
        }
        const auto cursor = page.envelope["data"].toObject()["nextCursor"].toString();
        QVERIFY(!cursor.isEmpty());
        QCOMPARE(fixture.invoke("read_page", {{"url", "https://example.com/article"}, {"cursor", cursor}}).envelope["state"].toString(), "completed");
    }

    void command_codeMarkup_hasReadableText() {
        Fixture fixture;
        const auto result = fixture.invoke("read_page", {{"url", "https://example.com/code"}});
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.envelope["state"].toString(), "completed");
        const auto content = result.envelope["data"].toObject()["content"].toString();
        QVERIFY(content.contains("Use print() now."));
        QVERIFY(content.contains("print('hello')"));
    }

    void package_readPageAcceptsRealArguments() {
        PluginCatalog catalog;
        QVERIFY(catalog.registerPlugin({QString(WEB_PACKAGE)}));
        const auto loaded = catalog.loadCapability({"desktop-pet.web", "desktop-pet.web.tools", "1.0.0", "read_page"});
        QVERIFY(loaded);
        const auto valid = SchemaValidator::validate(loaded.value().parameterSchema,
            QJsonObject{{"url", "https://docs.python.org/3/tutorial/"}});
        QVERIFY2(valid, qPrintable(valid ? QString() : valid.error().message));
    }

    void failedPayload_preservesDetailsAndRedactsSecrets() {
        ToolResultSanitizer sanitizer;
        auto failure = ToolResult::fail("ACTION_FAILED");
        failure.data = {{"stage", "failed"}, {"data", QJsonObject{
            {"error", QJsonObject{{"code", "NETWORK_ERROR"}, {"message", "Connection failed"}}},
            {"api_key", "fake-key"}, {"nested", QJsonArray{QJsonObject{
                {"token", "fake-token"}, {"password", "fake-password"}}}}}}};
        const auto payload = QJsonDocument::fromJson(sanitizer.toPayload(failure).toUtf8()).object();
        QVERIFY(!payload["success"].toBool());
        QCOMPARE(payload["error"].toString(), "ACTION_FAILED");
        const auto data = payload["data"].toObject()["data"].toObject();
        QCOMPARE(data["error"].toObject()["code"].toString(), "NETWORK_ERROR");
        QCOMPARE(data["error"].toObject()["message"].toString(), "Connection failed");
        QCOMPARE(data["api_key"].toString(), "[REDACTED]");
        const auto nested = data["nested"].toArray()[0].toObject();
        QCOMPARE(nested["token"].toString(), "[REDACTED]");
        QCOMPARE(nested["password"].toString(), "[REDACTED]");
        const auto plain = QJsonDocument::fromJson(sanitizer.toPayload(ToolResult::fail("plain error")).toUtf8()).object();
        QCOMPARE(plain.size(), 2);
        QVERIFY(!plain.contains("data"));
        QCOMPARE(plain["error"].toString(), "plain error");
    }

    void package_validSchemasAndDependencies() {
        for (const auto& pair : {qMakePair("plugin", "plugin.json"), qMakePair("capability", "tool.json"), qMakePair("skill", "skill.json")}) {
            const auto valid = SchemaValidator::validateManifest(pair.first, readJson(pair.second));
            QVERIFY2(valid, qPrintable(valid ? QString() : valid.error().message));
        }
        PluginCatalog catalog;
        const auto registered = catalog.registerPlugin({QString(WEB_PACKAGE)});
        QVERIFY2(registered, qPrintable(registered ? QString() : registered.error().message));
        QCOMPARE(registered.value().entries.size(), 2);
        const auto skill = catalog.loadCapability({"desktop-pet.web", "desktop-pet.web.skill", "1.0.0", {}});
        QVERIFY(skill);
        QCOMPARE(skill.value().dependencies.size(), 1);
        const auto dependency = skill.value().dependencies[0].toObject();
        QCOMPARE(dependency["id"].toString(), "desktop-pet.web.tools");
        QCOMPARE(dependency["version"].toString(), "1.0.0");
        QVERIFY(dependency["required"].toBool());
        for (const auto& operation : dependency["operations"].toArray())
            QVERIFY(catalog.loadCapability({"desktop-pet.web", dependency["id"].toString(), "1.0.0", operation.toString()}));
        QVERIFY(skill.value().instructions.contains(QString::fromUtf8("等待")));
    }

    void package_execution_digestUnchanged() {
        PluginCatalog catalog;
        QVERIFY(catalog.registerPlugin({QString(WEB_PACKAGE)}));
        const CapabilityRef ref{"desktop-pet.web", "desktop-pet.web.tools", "1.0.0", "read_page"};
        const auto before = catalog.loadCapability(ref);
        QVERIFY(before);
        Fixture fixture;
        QCOMPARE(fixture.invoke("status", {}, true).envelope["state"].toString(), "completed");
        QCOMPARE(fixture.invoke("search", {{"query", "Qt"}}).envelope["state"].toString(), "completed");
        QCOMPARE(fixture.invoke("read_page", {{"url", "https://example.com/article"}}).envelope["state"].toString(), "completed");
        const auto after = catalog.loadCapability(ref);
        QVERIFY(after);
        QCOMPARE(after.value().definitionDigest, before.value().definitionDigest);
    }
};

QTEST_GUILESS_MAIN(WebCommandTests)
#include "test_web_command.moc"
