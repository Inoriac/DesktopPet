#include "ai/plugin/plugin_catalog.h"
#include "ai/plugin/schema_validator.h"
#include "ai/tools/runtime/tool_result_sanitizer.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
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
