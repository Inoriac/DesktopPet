//
// LX Music runtime tools
//

#ifndef DESKTOP_PET_MUSIC_TOOLS_H
#define DESKTOP_PET_MUSIC_TOOLS_H

#include "../ai_tool.h"

#include <QEventLoop>
#include <QCoreApplication>
#include <QDir>
#include <QDesktopServices>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {
inline void waitForLxMusicMs(int timeoutMs) {
    QEventLoop loop;
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
}

inline ToolResult callLxMusicApiRaw(const QString& path, const QUrlQuery& query = {}, int timeoutMs = 3000) {
    QUrl url(QString("http://127.0.0.1:23330") + path);
    if (!query.isEmpty()) {
        url.setQuery(query);
    }
    if (!url.isValid()) {
        return ToolResult::fail("invalid_lx_music_api_url");
    }

    QNetworkRequest request(url);
    QNetworkAccessManager manager;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    QNetworkReply* reply = manager.get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timer.start(timeoutMs);
    loop.exec();

    if (timer.isActive()) {
        timer.stop();
    } else {
        reply->abort();
        reply->deleteLater();
        return ToolResult::fail("lx_music_api_timeout");
    }

    if (reply->error() != QNetworkReply::NoError) {
        const QString err = reply->errorString();
        reply->deleteLater();
        return ToolResult::fail(QString("lx_music_api_unreachable: %1").arg(err));
    }

    const QByteArray body = reply->readAll();
    reply->deleteLater();

    QJsonObject data;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        data = doc.object();
    } else if (doc.isArray()) {
        data["items"] = doc.array();
    } else {
        data["text"] = QString::fromUtf8(body);
    }
    data["api_path"] = path;

    return ToolResult::ok(data);
}

inline QStringList lxMusicExecutableCandidates() {
    QStringList candidates;
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    const QString appData = qEnvironmentVariable("APPDATA");
    const QString programFiles = qEnvironmentVariable("ProgramFiles");
    const QString programFilesX86 = qEnvironmentVariable("ProgramFiles(x86)");
    const QString home = QDir::homePath();

    candidates << localAppData + "/Programs/lx-music-desktop/lx-music-desktop.exe"
               << localAppData + "/Programs/lx-music-desktop/LX Music.exe"
               << localAppData + "/Programs/lx-music-desktop/LX Music Desktop.exe"
               << appData + "/lx-music-desktop/lx-music-desktop.exe"
               << appData + "/lx-music-desktop/LX Music.exe"
               << home + "/AppData/Local/Programs/lx-music-desktop/lx-music-desktop.exe"
               << home + "/AppData/Local/Programs/lx-music-desktop/LX Music.exe"
               << programFiles + "/lx-music-desktop/lx-music-desktop.exe"
               << programFiles + "/lx-music-desktop/LX Music.exe"
               << programFilesX86 + "/lx-music-desktop/lx-music-desktop.exe"
               << programFilesX86 + "/lx-music-desktop/LX Music.exe";
    candidates.removeDuplicates();
    candidates.removeAll("");
    return candidates;
}

inline QString findLxMusicExecutable() {
    for (const QString& candidate : lxMusicExecutableCandidates()) {
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

inline bool startDetachedQuiet(const QString& program, const QStringList& args = {}) {
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    return process.startDetached();
}

inline ToolResult launchLxMusicDesktop() {
    const QString executable = findLxMusicExecutable();
    if (!executable.isEmpty() && startDetachedQuiet(executable)) {
        return ToolResult::ok(QJsonObject{{"launched", true}, {"method", "executable"}, {"path", executable}});
    }

    if (executable.isEmpty()) {
        return ToolResult::fail("lx_music_executable_not_found");
    }
    return ToolResult::fail("failed_to_launch_lx_music_executable");
}

inline ToolResult waitForLxMusicApiReady(int timeoutMs = 12000) {
    const int stepMs = 600;
    int elapsed = 0;
    ToolResult last = ToolResult::fail("lx_music_api_not_checked");
    while (elapsed <= timeoutMs) {
        last = callLxMusicApiRaw("/status", {}, 1200);
        if (last.success) {
            last.data["api_ready"] = true;
            return last;
        }
        waitForLxMusicMs(stepMs);
        elapsed += stepMs;
    }
    return ToolResult::fail(QString("lx_music_api_not_ready_after_launch: %1").arg(last.errorMessage));
}

inline ToolResult ensureLxMusicApiReady() {
    ToolResult status = callLxMusicApiRaw("/status", {}, 1200);
    if (status.success) {
        status.data["api_ready"] = true;
        return status;
    }

    ToolResult launch = launchLxMusicDesktop();
    if (!launch.success) {
        return launch;
    }

    ToolResult ready = waitForLxMusicApiReady();
    if (ready.success) {
        ready.data["auto_launched"] = true;
        ready.data["launch"] = launch.data;
    }
    return ready;
}

inline ToolResult callLxMusicApi(const QString& path, const QUrlQuery& query = {}) {
    ToolResult result = callLxMusicApiRaw(path, query);
    if (result.success) {
        return result;
    }

    ToolResult ready = ensureLxMusicApiReady();
    if (!ready.success) {
        return ready;
    }

    result = callLxMusicApiRaw(path, query);
    if (result.success) {
        result.data["auto_launched"] = ready.data.value("auto_launched").toBool(false);
    }
    return result;
}

inline bool lxMusicTextMatches(const QString& actual, const QString& expected) {
    const QString trimmed = expected.trimmed();
    const QString current = actual.trimmed();
    if (trimmed.isEmpty()) {
        return true;
    }
    if (current.isEmpty()) {
        return false;
    }
    return current.contains(trimmed, Qt::CaseInsensitive) || trimmed.contains(current, Qt::CaseInsensitive);
}

inline ToolResult waitForLxMusicPlayback(const QString& expectedSong = {}, const QString& expectedArtist = {}, int timeoutMs = 15000) {
    const int stepMs = 700;
    int elapsed = 0;
    ToolResult last = ToolResult::fail("lx_music_playback_not_checked");
    while (elapsed <= timeoutMs) {
        last = callLxMusicApiRaw("/status", {}, 1200);
        if (last.success) {
            const QString status = last.data.value("status").toString();
            const QString name = last.data.value("name").toString();
            const QString singer = last.data.value("singer").toString();
            const bool playing = status.compare("playing", Qt::CaseInsensitive) == 0;
            const bool songMatches = lxMusicTextMatches(name, expectedSong);
            const bool artistMatches = lxMusicTextMatches(singer, expectedArtist);
            if (playing && songMatches && artistMatches) {
                last.data["playback_verified"] = true;
                return last;
            }
        }
        waitForLxMusicMs(stepMs);
        elapsed += stepMs;
    }

    if (last.success) {
        const QString status = last.data.value("status").toString();
        const QString name = last.data.value("name").toString();
        const QString singer = last.data.value("singer").toString();
        return ToolResult::fail(QString("lx_music_playback_not_verified: status=%1, current=%2 - %3")
            .arg(status, name, singer));
    }
    return ToolResult::fail(QString("lx_music_playback_not_verified: %1").arg(last.errorMessage));
}

inline ToolResult openLxMusicScheme(const QString& schemeUrl, bool verifyPlayback = false, const QString& expectedSong = {}, const QString& expectedArtist = {}) {
    if (schemeUrl.trimmed().isEmpty() || !schemeUrl.startsWith("lxmusic://")) {
        return ToolResult::fail("invalid_lx_music_scheme_url");
    }

    QJsonObject launchInfo;
    bool apiReadyBeforeOpen = callLxMusicApiRaw("/status", {}, 1200).success;
    const QString executable = findLxMusicExecutable();
    if (!apiReadyBeforeOpen && !executable.isEmpty()) {
        if (!startDetachedQuiet(executable)) {
            return ToolResult::fail("failed_to_launch_lx_music_executable");
        }
        ToolResult ready = waitForLxMusicApiReady();
        if (ready.success) {
            apiReadyBeforeOpen = true;
            launchInfo = QJsonObject{{"auto_launched", true}, {"method", "executable"}, {"path", executable}};
        } else if (verifyPlayback) {
            return ready;
        }
    }

    bool opened = false;
    QString openMethod;
    if (!executable.isEmpty()) {
        opened = startDetachedQuiet(executable, {schemeUrl});
        openMethod = "executable_argument";
    }
    if (!opened) {
        opened = QDesktopServices::openUrl(QUrl(schemeUrl));
        openMethod = "scheme";
    }
    if (!opened) {
        return ToolResult::fail("failed_to_open_lx_music_scheme");
    }

    QJsonObject openedData{{"url", schemeUrl}, {"opened", true}, {"open_method", openMethod}};
    if (apiReadyBeforeOpen) {
        openedData["api_ready_before_open"] = true;
    }
    if (!launchInfo.isEmpty()) {
        openedData["launch"] = launchInfo;
    }
    if (!verifyPlayback) {
        return ToolResult::ok(openedData);
    }

    ToolResult verified = waitForLxMusicPlayback(expectedSong, expectedArtist);
    if (!verified.success) {
        return verified;
    }
    openedData["playback_verified"] = true;
    openedData["status"] = verified.data;
    return ToolResult::ok(openedData);
}

inline QString lxPlaylistScriptPath() {
    const QString appDirCandidate = QCoreApplication::applicationDirPath()
        + "/core/ai/skills/lx-music-skill/scripts/playlist.py";
    if (QFile::exists(appDirCandidate)) {
        return appDirCandidate;
    }

    const QString cwdCandidate = QDir::currentPath()
        + "/core/ai/skills/lx-music-skill/scripts/playlist.py";
    if (QFile::exists(cwdCandidate)) {
        return cwdCandidate;
    }

    return "core/ai/skills/lx-music-skill/scripts/playlist.py";
}

inline ToolResult runLxPlaylistScript(const QStringList& args) {
    QProcess process;
    process.setProgram("python");
    process.setArguments(QStringList{lxPlaylistScriptPath()} + args);
    process.start();
    if (!process.waitForStarted(3000)) {
        return ToolResult::fail("failed_to_start_lx_playlist_script");
    }
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        return ToolResult::fail("lx_playlist_script_timeout");
    }

    const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
    const QByteArray stdoutBytes = process.readAllStandardOutput();
    if (process.exitCode() != 0) {
        return ToolResult::fail(stderrText.isEmpty() ? "lx_playlist_script_failed" : stderrText);
    }

    QJsonObject data;
    const QJsonDocument doc = QJsonDocument::fromJson(stdoutBytes);
    if (doc.isArray()) {
        data["items"] = doc.array();
    } else if (doc.isObject()) {
        data = doc.object();
    } else {
        data["text"] = QString::fromUtf8(stdoutBytes).trimmed();
    }
    return ToolResult::ok(data);
}
}

class LxMusicStatusTool : public AITool {
public:
    LxMusicStatusTool()
        : AITool("lx_music_status", "获取 LX Music 当前播放状态、歌曲名、歌手、进度和音量。", ToolCategory::Query) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        return callLxMusicApi("/status");
    }
};

class LxMusicLaunchTool : public AITool {
public:
    LxMusicLaunchTool()
        : AITool("lx_music_launch", "自动启动 LX Music 桌面版并等待开放 API 服务可用。", ToolCategory::Action) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        return ensureLxMusicApiReady();
    }
};

class LxMusicPlayTool : public AITool {
public:
    LxMusicPlayTool()
        : AITool("lx_music_play", "让 LX Music 开始/继续播放。", ToolCategory::Action) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        return callLxMusicApi("/play");
    }
};

class LxMusicPauseTool : public AITool {
public:
    LxMusicPauseTool()
        : AITool("lx_music_pause", "暂停 LX Music 播放。", ToolCategory::Action) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        return callLxMusicApi("/pause");
    }
};

class LxMusicSkipNextTool : public AITool {
public:
    LxMusicSkipNextTool()
        : AITool("lx_music_skip_next", "LX Music 切到下一首。", ToolCategory::Action) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        return callLxMusicApi("/skip-next");
    }
};

class LxMusicSkipPrevTool : public AITool {
public:
    LxMusicSkipPrevTool()
        : AITool("lx_music_skip_prev", "LX Music 切到上一首。", ToolCategory::Action) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        return callLxMusicApi("/skip-prev");
    }
};

class LxMusicLyricTool : public AITool {
public:
    LxMusicLyricTool()
        : AITool("lx_music_lyric", "获取 LX Music 当前歌曲歌词。", ToolCategory::Query) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        return callLxMusicApi("/lyric");
    }
};

class LxMusicVolumeTool : public AITool {
public:
    LxMusicVolumeTool()
        : AITool("lx_music_set_volume", "设置 LX Music 音量，volume 范围 0-100。", ToolCategory::Action) {}

    QJsonObject parameterSchema() const override {
        QJsonObject volumeProp;
        volumeProp["type"] = "integer";
        volumeProp["description"] = "音量，范围 0-100";

        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{{"volume", volumeProp}};
        schema["required"] = QJsonArray{"volume"};
        return schema;
    }

    ToolResult execute(const QJsonObject& params) override {
        const int volume = qBound(0, params.value("volume").toInt(), 100);
        QUrlQuery query;
        query.addQueryItem("volume", QString::number(volume));
        return callLxMusicApi("/volume", query);
    }
};

class LxMusicSearchPlayTool : public AITool {
public:
    LxMusicSearchPlayTool()
        : AITool("lx_music_search_play", "用 LX Music 搜索并播放指定歌曲，可传 song 和 artist。", ToolCategory::Action) {}

    QJsonObject parameterSchema() const override {
        QJsonObject songProp;
        songProp["type"] = "string";
        songProp["description"] = "歌曲名或搜索关键词";

        QJsonObject artistProp;
        artistProp["type"] = "string";
        artistProp["description"] = "歌手名，可选";

        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{{"song", songProp}, {"artist", artistProp}};
        schema["required"] = QJsonArray{"song"};
        return schema;
    }

    ToolResult execute(const QJsonObject& params) override {
        const QString song = params.value("song").toString().trimmed();
        const QString artist = params.value("artist").toString().trimmed();
        if (song.isEmpty()) {
            return ToolResult::fail("missing song");
        }
        const QString keyword = artist.isEmpty() ? song : QString("%1-%2").arg(song, artist);
        return openLxMusicScheme(
            "lxmusic://music/searchPlay/" + QString::fromUtf8(QUrl::toPercentEncoding(keyword)),
            true,
            song,
            artist);
    }
};

class LxMusicListPlaylistsTool : public AITool {
public:
    LxMusicListPlaylistsTool()
        : AITool("lx_music_list_playlists", "列出 LX Music 本地歌单。", ToolCategory::Query) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        return runLxPlaylistScript({});
    }
};

class LxMusicPlaylistSongsTool : public AITool {
public:
    LxMusicPlaylistSongsTool()
        : AITool("lx_music_playlist_songs", "查看指定 LX Music 歌单中的歌曲。", ToolCategory::Query) {}

    QJsonObject parameterSchema() const override {
        QJsonObject idProp;
        idProp["type"] = "string";
        idProp["description"] = "歌单 id";

        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{{"playlist_id", idProp}};
        schema["required"] = QJsonArray{"playlist_id"};
        return schema;
    }

    ToolResult execute(const QJsonObject& params) override {
        const QString playlistId = params.value("playlist_id").toString().trimmed();
        if (playlistId.isEmpty()) {
            return ToolResult::fail("missing playlist_id");
        }
        return runLxPlaylistScript({"songs", playlistId});
    }
};

class LxMusicPlayPlaylistTool : public AITool {
public:
    LxMusicPlayPlaylistTool()
        : AITool("lx_music_play_playlist", "播放指定 LX Music 歌单。", ToolCategory::Action) {}

    QJsonObject parameterSchema() const override {
        QJsonObject idProp;
        idProp["type"] = "string";
        idProp["description"] = "歌单 id";

        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{{"playlist_id", idProp}};
        schema["required"] = QJsonArray{"playlist_id"};
        return schema;
    }

    ToolResult execute(const QJsonObject& params) override {
        const QString playlistId = params.value("playlist_id").toString().trimmed();
        if (playlistId.isEmpty()) {
            return ToolResult::fail("missing playlist_id");
        }
        ToolResult result = runLxPlaylistScript({"url", playlistId});
        if (!result.success) {
            return result;
        }
        const QString url = result.data.value("url").toString().trimmed();
        if (url.isEmpty()) {
            return ToolResult::fail("lx_music_playlist_url_not_found");
        }

        return openLxMusicScheme(url, true);
    }
};

#endif // DESKTOP_PET_MUSIC_TOOLS_H
