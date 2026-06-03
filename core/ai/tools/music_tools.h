//
// Local music control tools (Windows 网易云本地服务)
//

#ifndef DESKTOP_PET_MUSIC_TOOLS_H
#define DESKTOP_PET_MUSIC_TOOLS_H

#include "../ai_tool.h"
#include "configLoader/config_manager.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace {
inline ToolResult callLocalMusicService(const QString& command, const QJsonObject& args) {
    const MusicControlConfig& cfg = ConfigManager::instance().getMusicControlConfig();
    if (!cfg.enabled) {
        return ToolResult::fail("music_control_disabled");
    }
    if (cfg.provider != "netease_windows") {
        return ToolResult::fail("unsupported_music_provider");
    }

    const QUrl url(cfg.serviceBaseUrl + "/command");
    if (!url.isValid()) {
        return ToolResult::fail("invalid_music_service_url");
    }

    QJsonObject payload;
    payload["command"] = command;
    payload["args"] = args;
    payload["request_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkAccessManager manager;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    QNetworkReply* reply = manager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timer.start(cfg.requestTimeoutMs);
    loop.exec();

    if (timer.isActive()) {
        timer.stop();
    } else {
        reply->abort();
        reply->deleteLater();
        return ToolResult::fail("music_service_timeout");
    }

    if (reply->error() != QNetworkReply::NoError) {
        const QString err = reply->errorString();
        reply->deleteLater();
        return ToolResult::fail(QString("music_service_unreachable: %1").arg(err));
    }

    const QByteArray body = reply->readAll();
    reply->deleteLater();

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        return ToolResult::fail("invalid_music_service_response");
    }

    const QJsonObject obj = doc.object();
    if (!obj.value("success").toBool(false)) {
        return ToolResult::fail(obj.value("error").toString("music_command_failed"));
    }

    return ToolResult::ok(obj);
}
}

class MusicNextTrackTool : public AITool {
public:
    MusicNextTrackTool()
        : AITool(
            "music_next_track",
            "切到下一首歌曲。仅在本地 Windows 网易云客户端可用。",
            ToolCategory::Action
          ) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        return callLocalMusicService("next_track", QJsonObject{});
    }
};

class MusicPlaySongTool : public AITool {
public:
    MusicPlaySongTool()
        : AITool(
            "music_play_song",
            "播放指定歌曲。优先 song_id/song_url 直连播放；否则使用 song+artist 搜索精确匹配，失败降级第一条。",
            ToolCategory::Action
          ) {}

    QJsonObject parameterSchema() const override {
        QJsonObject songProp;
        songProp["type"] = "string";
        songProp["description"] = "歌曲名";

        QJsonObject artistProp;
        artistProp["type"] = "string";
        artistProp["description"] = "歌手名";

        QJsonObject songIdProp;
        songIdProp["type"] = "string";
        songIdProp["description"] = "网易云歌曲ID（可选，优先）";

        QJsonObject songUrlProp;
        songUrlProp["type"] = "string";
        songUrlProp["description"] = "网易云歌曲URL（可选，优先）";

        QJsonObject props;
        props["song"] = songProp;
        props["artist"] = artistProp;
        props["song_id"] = songIdProp;
        props["song_url"] = songUrlProp;

        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = props;
        return schema;
    }

    bool validate(const QJsonObject& params) const override {
        const bool hasSongId = params.contains("song_id") && !params.value("song_id").toString().trimmed().isEmpty();
        const bool hasSongUrl = params.contains("song_url") && !params.value("song_url").toString().trimmed().isEmpty();
        const bool hasSong = params.contains("song") && !params.value("song").toString().trimmed().isEmpty();

        if (hasSongId || hasSongUrl) return true;
        return hasSong;
    }

    ToolResult execute(const QJsonObject& params) override {
        QJsonObject args;
        args["song"] = params.value("song").toString();
        args["artist"] = params.value("artist").toString();
        args["song_id"] = params.value("song_id").toString();
        args["song_url"] = params.value("song_url").toString();
        return callLocalMusicService("play_song", args);
    }
};

class MusicSwitchPlaylistTool : public AITool {
public:
    MusicSwitchPlaylistTool()
        : AITool(
            "music_switch_playlist",
            "按歌单ID切换网易云歌单。",
            ToolCategory::Action
          ) {}

    QJsonObject parameterSchema() const override {
        QJsonObject idProp;
        idProp["type"] = "string";
        idProp["description"] = "目标歌单ID";

        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{{"playlist_id", idProp}};
        schema["required"] = QJsonArray{"playlist_id"};
        return schema;
    }

    ToolResult execute(const QJsonObject& params) override {
        QJsonObject args;
        args["playlist_id"] = params.value("playlist_id").toString();
        return callLocalMusicService("switch_playlist", args);
    }
};

#endif // DESKTOP_PET_MUSIC_TOOLS_H
