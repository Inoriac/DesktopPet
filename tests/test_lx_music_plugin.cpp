#include "ai/plugin/plugin_runtime.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>

namespace {
const QString pluginId = "desktop-pet.lx-music";
const QString toolId = pluginId + ".tools";
const QString skillId = pluginId + ".skill";
const QString profileId = "12345678-1234-4234-8234-123456789abc";

QJsonObject readJson(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

struct Fixture {
    QTemporaryDir directory{QDir::tempPath() + "/lx plugin 中文 XXXXXX"};
    QProcess backend;
    QJsonObject endpoint;
    QString error;
    QString profilePath() const { return directory.filePath("profiles/" + profileId); }
    QString hostPath() const { return profilePath() + "/plugins.json"; }

    bool start() {
        if (!directory.isValid() || !QDir().mkpath(profilePath()))
            return false;
        backend.start(QString::fromUtf8(LX_MUSIC_PYTHON),
                      {QString::fromUtf8(LX_MUSIC_BACKEND), "--directory", directory.path()});
        if (!backend.waitForStarted(5000) || !backend.waitForReadyRead(5000)) {
            error = QString::fromUtf8(backend.readAllStandardError());
            return false;
        }
        QByteArray line;
        while (!backend.canReadLine() && backend.waitForReadyRead(1000)) {}
        line = backend.readLine();
        endpoint = QJsonDocument::fromJson(line).object();
        return endpoint.contains("apiUrl") && configure("off");
    }

    bool configure(const QString &autonomous) {
        QProcess process;
        process.start(QString::fromUtf8(LX_MUSIC_PYTHON),
                      {QString::fromUtf8(LX_MUSIC_PACKAGE) + "/configure.py",
                       "--profile-dir", profilePath(), "--database", endpoint["database"].toString(),
                       "--api-url", endpoint["apiUrl"].toString(), "--autonomous", autonomous, "--apply"});
        if (!process.waitForFinished(10000) || process.exitCode() != 0) {
            error = QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
            return false;
        }
        return readJson(hostPath()).contains("configuration");
    }

    QJsonObject counts() const { return readJson(directory.filePath("counts.json")); }
    void setMode(const QByteArray &mode) {
        QFile file(directory.filePath("mode.txt"));
        if (file.open(QIODevice::WriteOnly))
            file.write(mode);
    }
    ~Fixture() {
        backend.kill();
        backend.waitForFinished(5000);
    }
};

CapabilityRef reference(const QString &operation) {
    return {pluginId, toolId, "1.0.0", operation};
}

ToolExecutionOutcome gateway(PluginRuntime &runtime, const ContextSpec &context,
                             const QString &name, const QJsonObject &arguments = {},
                             bool autonomous = false) {
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    auto outcome = std::make_shared<std::optional<ToolExecutionOutcome>>();
    ToolExecutionRequest request;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.activityId = "music-integration";
    request.context = context;
    request.toolName = name;
    request.arguments = arguments;
    request.autonomous = autonomous;
    request.timeoutMs = 5000;
    QPointer<QEventLoop> guard(&loop);
    runtime.executeGateway(request, [outcome, guard](ToolExecutionOutcome value) {
        if (guard)
            QMetaObject::invokeMethod(guard, [outcome, guard, value = std::move(value)]() mutable {
                *outcome = std::move(value);
                if (guard) guard->quit();
            }, Qt::QueuedConnection);
    });
    deadline.start(10000);
    loop.exec();
    if (*outcome)
        return **outcome;
    ToolExecutionOutcome failed;
    failed.result = ToolResult::fail("Integration gateway wait timed out");
    return failed;
}

ToolExecutionOutcome invoke(PluginRuntime &runtime, const ContextSpec &context,
                            const QString &operation, const QJsonObject &arguments = {},
                            bool autonomous = false) {
    auto loaded = gateway(runtime, context, "load_capability", {{"ref", reference(operation).toJson()}}, autonomous);
    if (!loaded.result.success)
        return loaded;
    return gateway(runtime, context, "invoke_capability",
                   {{"ref", reference(operation).toJson()}, {"arguments", arguments}}, autonomous);
}
}

class LxMusicPluginTests : public QObject {
    Q_OBJECT
private slots:
    void real_player_read_only_gateway() {
        const QString hostPath = qEnvironmentVariable("DESKTOP_PET_LX_ACCEPTANCE_HOST");
        if (hostPath.isEmpty())
            QSKIP("Opt-in real player acceptance: set DESKTOP_PET_LX_ACCEPTANCE_HOST to an isolated configured profile");
        const QString liveProfile = QFileInfo(hostPath).dir().dirName();
        PluginRuntime runtime(liveProfile, hostPath);
        const auto initialized = runtime.initialize();
        QVERIFY2(initialized, qPrintable(initialized ? QString() : initialized.error().message));
        const auto context = ContextSpec::localOwner(liveProfile);
        QVERIFY(gateway(runtime, context, "discover_capabilities").result.success);
        const CapabilityRef skill{pluginId, skillId, "1.0.0", {}};
        QVERIFY(gateway(runtime, context, "load_capability", {{"ref", skill.toJson()}}).result.success);
        const auto tool = gateway(runtime, context, "load_capability", {{"ref", reference({}).toJson()}});
        QVERIFY(tool.result.success);
        QCOMPARE(tool.result.data["operations"].toArray().size(), 12);
        QJsonArray playlists;
        for (const QString &operation : {QString("status"), QString("lyric"), QString("list_playlists")}) {
            const auto result = invoke(runtime, context, operation);
            qInfo().noquote() << operation << QJsonDocument(result.result.data).toJson(QJsonDocument::Compact);
            QVERIFY2(result.result.success, qPrintable(result.result.errorMessage));
            QVERIFY(result.executed);
            QCOMPARE(result.result.data["stage"].toString(), "completed");
            if (operation == "list_playlists")
                playlists = result.result.data["data"].toObject()["items"].toArray();
        }
        if (!playlists.isEmpty()) {
            const auto result = invoke(runtime, context, "playlist_songs",
                                       {{"playlist_id", playlists[0].toObject()["id"]}});
            QVERIFY2(result.result.success, qPrintable(result.result.errorMessage));
            QCOMPARE(result.result.data["stage"].toString(), "completed");
            qInfo() << "playlist_songs count" << result.result.data["data"].toObject()["items"].toArray().size();
        }
    }

    void runtime_discover_load_skill_and_tools() {
        Fixture fixture;
        QVERIFY2(fixture.start(), qPrintable(fixture.error));
        PluginRuntime runtime(profileId, fixture.hostPath());
        QVERIFY(runtime.initialize());
        const auto context = ContextSpec::localOwner(profileId);
        const auto discovered = gateway(runtime, context, "discover_capabilities");
        QVERIFY(discovered.result.success);
        QCOMPARE(discovered.result.data["entries"].toArray().size(), 2);
        QVERIFY(!QJsonDocument(discovered.result.data).toJson().contains("instructions"));
        const CapabilityRef skill{pluginId, skillId, "1.0.0", {}};
        const auto method = gateway(runtime, context, "load_capability", {{"ref", skill.toJson()}});
        QVERIFY2(method.result.success, qPrintable(method.result.errorMessage));
        QCOMPARE(method.result.data["kind"].toString(), "skill");
        QVERIFY(method.result.data["instructions"].toString().contains("unknown"));
        QCOMPARE(method.result.data["dependencies"].toArray()[0].toObject()["operations"].toArray().size(), 12);
        const auto tool = gateway(runtime, context, "load_capability", {{"ref", reference({}).toJson()}});
        QVERIFY(tool.result.success);
        QCOMPARE(tool.result.data["operations"].toArray().size(), 12);
        for (const auto &item : tool.result.data["operations"].toArray()) {
            const auto id = item.toObject()["id"].toString();
            QVERIFY(!id.isEmpty());
            const auto loaded = gateway(runtime, context, "load_capability", {{"ref", reference(id).toJson()}});
            QVERIFY2(loaded.result.success, qPrintable(id + ": " + loaded.result.errorMessage));
            QVERIFY(loaded.result.data["parameters"].isObject());
        }
        QVERIFY(fixture.counts().isEmpty());
    }

    void runtime_command_http_and_sqlite() {
        Fixture fixture;
        QVERIFY2(fixture.start(), qPrintable(fixture.error));
        PluginRuntime runtime(profileId, fixture.hostPath());
        QVERIFY(runtime.initialize());
        const auto context = ContextSpec::localOwner(profileId);
        const auto status = invoke(runtime, context, "status");
        QVERIFY2(status.result.success, qPrintable(status.result.errorMessage));
        QVERIFY(status.executed);
        QCOMPARE(status.result.data["stage"].toString(), "completed");
        QCOMPARE(status.result.data["data"].toObject()["status"].toObject()["name"].toString(), QString::fromUtf8("晴天"));
        const auto lists = invoke(runtime, context, "list_playlists");
        QVERIFY(lists.result.success);
        const auto items = lists.result.data["data"].toObject()["items"].toArray();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items[0].toObject()["id"].toString(), "empty");
        const auto songs = invoke(runtime, context, "playlist_songs", {{"playlist_id", "cloud"}});
        QVERIFY(songs.result.success);
        QCOMPARE(songs.result.data["data"].toObject()["items"].toArray()[0].toObject()["name"].toString(), QString::fromUtf8("稻香"));
        const auto lyric = invoke(runtime, context, "lyric");
        QVERIFY(lyric.result.success);
        QVERIFY(lyric.result.data["data"].toObject()["lyric"].toString().contains(QString::fromUtf8("麻雀")));
        QVERIFY(invoke(runtime, context, "play").result.success);
        QVERIFY(invoke(runtime, context, "pause").result.success);
        const auto volume = invoke(runtime, context, "set_volume", {{"volume", 27}});
        QVERIFY(volume.result.success);
        QCOMPARE(volume.result.data["data"].toObject()["status"].toObject()["volume"].toInt(), 27);
        QCOMPARE(fixture.counts()["/play"].toInt(), 1);
        QCOMPARE(fixture.counts()["/pause"].toInt(), 1);
        QCOMPARE(fixture.counts()["/volume"].toInt(), 1);
    }

    void runtime_local_and_internal_permissions() {
        Fixture fixture;
        QVERIFY2(fixture.start(), qPrintable(fixture.error));
        PluginRuntime runtime(profileId, fixture.hostPath());
        QVERIFY(runtime.initialize());
        ContextSpec internal;
        internal.profileId = profileId;
        internal.activityId = "autonomous-music";
        QVERIFY(runtime.discoverCapabilities(internal, true).isEmpty());
        QVERIFY(!invoke(runtime, internal, "status", {}, true).result.success);
        QVERIFY(fixture.counts().isEmpty());
        QVERIFY2(fixture.configure("read"), qPrintable(fixture.error));
        QVERIFY(runtime.initialize());
        QCOMPARE(runtime.discoverCapabilities(internal, true).size(), 2);
        QVERIFY(runtime.loadCapability({pluginId, skillId, "1.0.0", {}}, internal, true));
        QVERIFY(invoke(runtime, internal, "status", {}, true).result.success);
        QVERIFY(!invoke(runtime, internal, "play", {}, true).result.success);
        const auto blocked = gateway(runtime, internal, "invoke_capability",
                                     {{"ref", reference("play").toJson()}, {"arguments", QJsonObject{}}}, true);
        QVERIFY(!blocked.result.success);
        QVERIFY(!blocked.executed);
        QCOMPARE(fixture.counts()["/play"].toInt(), 0);
        QVERIFY(invoke(runtime, ContextSpec::localOwner(profileId), "play").result.success);
        QCOMPARE(fixture.counts()["/play"].toInt(), 1);
        QVERIFY2(fixture.configure("all"), qPrintable(fixture.error));
        QVERIFY(runtime.initialize());
        QVERIFY(invoke(runtime, internal, "play", {}, true).result.success);
        QCOMPARE(fixture.counts()["/play"].toInt(), 2);
    }

    void runtime_external_denied() {
        Fixture fixture;
        QVERIFY2(fixture.start(), qPrintable(fixture.error));
        QVERIFY(fixture.configure("all"));
        PluginRuntime runtime(profileId, fixture.hostPath());
        QVERIFY(runtime.initialize());
        auto context = ContextSpec::localOwner(profileId);
        context.originKind = OriginKind::External;
        context.conversationId = "external-room";
        QVERIFY(runtime.discoverCapabilities(context).isEmpty());
        QVERIFY(!runtime.loadCapability(reference("status"), context));
        const auto blocked = gateway(runtime, context, "invoke_capability",
                                     {{"ref", reference("play").toJson()}, {"arguments", QJsonObject{}}});
        QVERIFY(!blocked.result.success);
        QVERIFY(!blocked.executed);
        QVERIFY(fixture.counts().isEmpty());
    }

    void runtime_invalid_disabled_stale_no_write() {
        Fixture fixture;
        QVERIFY2(fixture.start(), qPrintable(fixture.error));
        PluginRuntime runtime(profileId, fixture.hostPath());
        QVERIFY(runtime.initialize());
        const auto context = ContextSpec::localOwner(profileId);
        const auto invalid = invoke(runtime, context, "set_volume", {{"volume", 101}});
        QVERIFY(!invalid.result.success);
        QVERIFY(!invalid.executed);
        QVERIFY(runtime.loadCapability(reference("play"), context));
        QVERIFY(runtime.setPluginEnabled({pluginId, false, runtime.catalogRevision()}));
        const auto disabled = gateway(runtime, context, "invoke_capability",
                                      {{"ref", reference("play").toJson()}, {"arguments", QJsonObject{}}});
        QVERIFY(!disabled.result.success);
        QVERIFY(!disabled.executed);
        QVERIFY(runtime.setPluginEnabled({pluginId, true, runtime.catalogRevision()}));
        QVERIFY(runtime.loadCapability(reference("play"), context));
        QVERIFY(runtime.setHostConfiguration(readJson(fixture.hostPath())));
        const auto stale = gateway(runtime, context, "invoke_capability",
                                   {{"ref", reference("play").toJson()}, {"arguments", QJsonObject{}}});
        QVERIFY(!stale.result.success);
        QVERIFY(!stale.executed);
        auto oldVersion = reference("play");
        oldVersion.entryVersion = "0.9.0";
        QVERIFY(!runtime.loadCapability(oldVersion, context));
        QVERIFY(fixture.counts().isEmpty());
    }

    void runtime_unknown_single_dispatch() {
        Fixture fixture;
        QVERIFY2(fixture.start(), qPrintable(fixture.error));
        fixture.setMode("disconnect");
        PluginRuntime runtime(profileId, fixture.hostPath());
        QVERIFY(runtime.initialize());
        const auto context = ContextSpec::localOwner(profileId);
        const auto outcome = invoke(runtime, context, "play");
        QVERIFY(!outcome.result.success);
        QVERIFY(outcome.executed);
        QCOMPARE(outcome.result.data["stage"].toString(), "unknown");
        QCOMPARE(outcome.result.data["effectCertainty"].toString(), "unknown");
        QVERIFY(!outcome.result.data["data"].toObject()["error"].toObject()["code"].toString().isEmpty());
        QCOMPARE(fixture.counts()["/play"].toInt(), 1);
        const auto verification = invoke(runtime, context, "status");
        QVERIFY(verification.result.success);
        QCOMPARE(verification.result.data["data"].toObject()["status"].toObject()["status"].toString(), "playing");
        QCOMPARE(fixture.counts()["/play"].toInt(), 1);
    }
};

QTEST_GUILESS_MAIN(LxMusicPluginTests)
#include "test_lx_music_plugin.moc"
