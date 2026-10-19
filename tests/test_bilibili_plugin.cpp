#include "ai/plugin/plugin_runtime.h"
#include "ai/tools/runtime/tool_result_sanitizer.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>

namespace {
const QString pluginId = "desktop-pet.bilibili";
const QString toolId = pluginId + ".tools";
const QString skillId = pluginId + ".skill";
const QString profileId = "12345678-1234-4234-8234-123456789abc";
const QString bvid = "BV1xx411c7mD";

QJsonObject readJson(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

struct Fixture {
    QTemporaryDir directory{QDir::tempPath() + "/bili plugin 中文 XXXXXX"};
    QString error;
    QString profilePath() const { return directory.filePath(profileId); }
    QString hostPath() const { return profilePath() + "/plugins.json"; }
    QString statePath() const { return directory.filePath("state"); }
    bool configure(bool asr = false, QString autonomous = "on") {
        QProcess process;
        QStringList args{"-B", QString(BILIBILI_BACKEND), "--prepare-profile", profilePath(),
                         "--state-dir", statePath(), "--autonomous", autonomous};
        if (asr) args.append("--asr");
        process.start(QString(BILIBILI_PYTHON), args);
        if (!process.waitForFinished(10000) || process.exitCode() != 0) {
            error = QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
            return false;
        }
        return readJson(hostPath()).contains("configuration");
    }
    int calls() const {
        QFile file(statePath() + "/backend-calls.jsonl");
        return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
    }
};

CapabilityRef ref(QString operation = {}) { return {pluginId, toolId, "1.0.0", operation}; }
ToolExecutionOutcome gateway(PluginRuntime &runtime, const ContextSpec &context, QString name,
                             QJsonObject arguments = {}, bool autonomous = false) {
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    auto output = std::make_shared<std::optional<ToolExecutionOutcome>>();
    ToolExecutionRequest request;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.activityId = "bili-integration";
    request.context = context;
    request.toolName = name;
    request.arguments = arguments;
    request.autonomous = autonomous;
    request.timeoutMs = 5000;
    QPointer<QEventLoop> guard(&loop);
    runtime.executeGateway(request, [output, guard](ToolExecutionOutcome value) {
        if (guard) QMetaObject::invokeMethod(guard, [output, guard, value = std::move(value)]() mutable {
            *output = std::move(value);
            if (guard) guard->quit();
        }, Qt::QueuedConnection);
    });
    deadline.start(10000);
    loop.exec();
    if (*output) return **output;
    ToolExecutionOutcome failed;
    failed.result = ToolResult::fail("Gateway timed out");
    return failed;
}
ToolExecutionOutcome invoke(PluginRuntime &runtime, const ContextSpec &context, QString operation,
                            QJsonObject arguments = {}, bool autonomous = false) {
    const auto loaded = gateway(runtime, context, "load_capability", {{"ref", ref(operation).toJson()}}, autonomous);
    if (!loaded.result.success) return loaded;
    return gateway(runtime, context, "invoke_capability",
                   {{"ref", ref(operation).toJson()}, {"arguments", arguments}}, autonomous);
}
}

class BilibiliPluginTests : public QObject {
    Q_OBJECT
private slots:
    void discovery_optional_asr_and_paginated_real_command_output() {
        Fixture fixture;
        QVERIFY2(fixture.configure(), qPrintable(fixture.error));
        PluginRuntime runtime(profileId, fixture.hostPath());
        const auto initialized = runtime.initialize();
        QVERIFY2(initialized, qPrintable(initialized ? QString() : initialized.error().message));
        const auto context = ContextSpec::localOwner(profileId);
        auto discovered = gateway(runtime, context, "discover_capabilities");
        QVERIFY(discovered.result.success);
        QCOMPARE(discovered.result.data["entries"].toArray().size(), 2);
        QVERIFY(!QJsonDocument(discovered.result.data).toJson().contains("instructions"));
        const CapabilityRef skill{pluginId, skillId, "1.0.0", {}};
        const auto method = gateway(runtime, context, "load_capability", {{"ref", skill.toJson()}});
        QVERIFY2(method.result.success, qPrintable(method.result.errorMessage));
        QVERIFY(method.result.data["instructions"].toString().contains(QString::fromUtf8("等待")));
        const auto tools = gateway(runtime, context, "load_capability", {{"ref", ref().toJson()}});
        QVERIFY2(tools.result.success, qPrintable(tools.result.errorMessage));
        QCOMPARE(tools.result.data["operations"].toArray().size(), 7); // catalog lists the complete package; invocation checks grants
        QCOMPARE(fixture.calls(), 0);
        QVERIFY(!invoke(runtime, context, "start_transcription", {{"bvid", bvid}}).result.success);
        QVERIFY(invoke(runtime, context, "status").result.success);
        auto search = invoke(runtime, context, "search", {{"keyword", QString::fromUtf8("宇宙🙂")}});
        QVERIFY2(search.result.success, qPrintable(search.result.errorMessage));
        QCOMPARE(search.result.data["data"].toObject()["entries"].toArray()[0].toObject()["bvid"].toString(), bvid);
        auto info = invoke(runtime, context, "video_info", {{"bvid", bvid}, {"part", 2}});
        QVERIFY(info.result.success);
        QCOMPARE(info.result.data["data"].toObject()["cid"].toInt(), 202);
        QVERIFY(invoke(runtime, context, "comments", {{"bvid", bvid}}).result.success);
        QString text, cursor;
        int pages = 0;
        do {
            QJsonObject args{{"bvid", bvid}, {"part", 2}};
            if (!cursor.isEmpty()) args["cursor"] = cursor;
            auto page = invoke(runtime, context, "read_transcript", args);
            QVERIFY2(page.result.success, qPrintable(page.result.errorMessage));
            QVERIFY(page.executed);
            ToolResultSanitizer sanitizer;
            const auto payload = sanitizer.toPayload(page.result);
            QVERIFY(payload.size() <= 6000);
            QVERIFY(!payload.contains("Tool result omitted"));
            const auto data = QJsonDocument::fromJson(payload.toUtf8()).object()["data"].toObject()["data"].toObject();
            QCOMPARE(data["source"].toString(), "platform_subtitle");
            QCOMPARE(data["part"].toInt(), 2);
            for (const auto &value : data["segments"].toArray()) {
                auto segment = value.toObject();
                QCOMPARE(segment["startMs"].toInt(), 1250);
                QCOMPARE(segment["endMs"].toInt(), 5000);
                text += segment["text"].toString();
            }
            cursor = data["nextCursor"].toString();
            QVERIFY(++pages < 20);
        } while (!cursor.isEmpty());
        QVERIFY(pages > 1);
        QCOMPARE(text, QString::fromUtf8("字幕🙂").repeated(900));
        QCOMPARE(fixture.calls(), 4); // search, info, comments, first subtitle page only
        QVERIFY(!QFile::exists(QString(BILIBILI_PACKAGE) + "/__pycache__"));
    }

    void internal_off_external_and_disabled_are_rejected_before_execution() {
        Fixture fixture;
        QVERIFY2(fixture.configure(false, "off"), qPrintable(fixture.error));
        PluginRuntime runtime(profileId, fixture.hostPath());
        QVERIFY(runtime.initialize());
        ContextSpec internal;
        internal.profileId = profileId;
        internal.activityId = "free-reading";
        QVERIFY(runtime.discoverCapabilities(internal, true).isEmpty());
        QVERIFY(!invoke(runtime, internal, "search", {{"keyword", "space"}}, true).result.success);
        QVERIFY(fixture.configure());
        QVERIFY(runtime.initialize());
        QCOMPARE(runtime.discoverCapabilities(internal, true).size(), 2);
        ContextSpec external = internal;
        external.originKind = OriginKind::External;
        external.conversationId = "external-conversation";
        QVERIFY(!invoke(runtime, external, "search", {{"keyword", "space"}}, true).result.success);
        QVERIFY(runtime.loadCapability(ref("search"), internal, true));
        QVERIFY(runtime.setPluginEnabled({pluginId, false, runtime.catalogRevision()}));
        const auto denied = gateway(runtime, internal, "invoke_capability",
                                   {{"ref", ref("search").toJson()}, {"arguments", QJsonObject{{"keyword", "space"}}}}, true);
        QVERIFY(!denied.result.success);
        QVERIFY(!denied.executed);
        QCOMPARE(fixture.calls(), 0);
    }

    void asr_receipt_status_and_source_through_real_gateway() {
        Fixture fixture;
        QVERIFY2(fixture.configure(true), qPrintable(fixture.error));
        PluginRuntime runtime(profileId, fixture.hostPath());
        QVERIFY(runtime.initialize());
        const auto context = ContextSpec::localOwner(profileId);
        const auto tool = gateway(runtime, context, "load_capability", {{"ref", ref().toJson()}});
        QVERIFY(tool.result.success);
        QCOMPARE(tool.result.data["operations"].toArray().size(), 7);
        for (const auto &item : tool.result.data["operations"].toArray())
            QVERIFY(gateway(runtime, context, "load_capability", {{"ref", ref(item.toObject()["id"].toString()).toJson()}}).result.success);
        auto started = invoke(runtime, context, "start_transcription", {{"bvid", bvid}, {"part", 2}});
        QVERIFY2(started.result.success, qPrintable(started.result.errorMessage));
        QCOMPARE(started.result.data["stage"].toString(), "accepted");
        const auto job = started.result.data["data"].toObject()["jobId"].toString();
        QVERIFY(!job.isEmpty());
        QCOMPARE(started.result.data["receipt"].toObject()["jobId"].toString(), job);
        QCOMPARE(started.result.data["tracking"].toObject()["operation"].toString(), "transcription_status");
        QString status;
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 10000) {
            auto queried = invoke(runtime, context, "transcription_status", {{"job_id", job}});
            QVERIFY(queried.result.success);
            status = queried.result.data["data"].toObject()["status"].toString();
            if (status == "completed" || status == "failed" || status == "unknown") break;
            QTest::qWait(50);
        }
        QCOMPARE(status, "completed");
        auto read = invoke(runtime, context, "read_transcript", {{"bvid", bvid}, {"part", 2}, {"source", "local_asr"}});
        QVERIFY(read.result.success);
        QCOMPARE(read.result.data["data"].toObject()["source"].toString(), "local_asr");
        QCOMPARE(read.result.data["data"].toObject()["segments"].toArray()[0].toObject()["text"].toString(), QString::fromUtf8("开始🙂"));
    }
};

QTEST_GUILESS_MAIN(BilibiliPluginTests)
#include "test_bilibili_plugin.moc"
