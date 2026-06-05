#include "intent_router.h"

#include <QStringList>

IntentRoute IntentRouter::route(const QString& input, const QString& triggerTag) const {
    Q_UNUSED(triggerTag)

    const QString normalized = normalize(input);
    if (normalized.isEmpty()) {
        return IntentRoute::needClarification("你想让我做什么？", "empty_input");
    }

    if (containsAny(normalized, {"删除文件", "删除目录", "格式化", "读取密码", "读取密钥"})) {
        return IntentRoute::rejected("这个请求涉及高风险或敏感操作，默认不会执行。");
    }

    if (containsAny(normalized, {"几点", "现在时间", "当前时间", "今天几号", "星期几"})) {
        return IntentRoute::directToolCall("get_current_time", {}, "time_query", 0.95);
    }

    if (containsAny(normalized, {"lx music下一首", "lxmusic下一首", "lx下一首", "lx music切歌", "lxmusic切歌"})) {
        return IntentRoute::directToolCall("lx_music_skip_next", {}, "lx_music_skip_next", 0.95);
    }

    if (containsAny(normalized, {"lx music上一首", "lxmusic上一首", "lx上一首"})) {
        return IntentRoute::directToolCall("lx_music_skip_prev", {}, "lx_music_skip_prev", 0.95);
    }

    if (containsAny(normalized, {"暂停lx", "lx music暂停", "lxmusic暂停", "暂停 lx music"})) {
        return IntentRoute::directToolCall("lx_music_pause", {}, "lx_music_pause", 0.95);
    }

    if (containsAny(normalized, {"播放lx", "lx music播放", "lxmusic播放", "继续 lx music"})) {
        return IntentRoute::directToolCall("lx_music_play", {}, "lx_music_play", 0.95);
    }

    if (containsAny(normalized, {"lx music状态", "lxmusic状态", "lx现在播放", "lx music现在播放", "lx music播放状态"})) {
        return IntentRoute::directToolCall("lx_music_status", {}, "lx_music_status", 0.95);
    }

    if (containsAny(normalized, {"lx music歌词", "lxmusic歌词", "lx当前歌词"})) {
        return IntentRoute::directToolCall("lx_music_lyric", {}, "lx_music_lyric", 0.95);
    }

    if (containsAny(normalized, {"lx music歌单", "lxmusic歌单", "lx歌单列表", "列出lx歌单"})) {
        return IntentRoute::directToolCall("lx_music_list_playlists", {}, "lx_music_list_playlists", 0.9);
    }

    if (containsAny(normalized, {"下一首", "下首歌", "切歌", "换首歌"})) {
        return IntentRoute::directToolCall("music_next_track", {}, "music_next_track", 0.9);
    }

    if (containsAny(normalized, {"你好", "hello", "嗨"}) && normalized.size() <= 12) {
        return IntentRoute::directReply("在哦。", "simple_greeting");
    }

    return IntentRoute::needLlm("complex_or_unknown_intent", 0.4);
}

bool IntentRouter::containsAny(const QString& normalizedInput, const QStringList& keywords) const {
    for (const QString& keyword : keywords) {
        if (normalizedInput.contains(keyword, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

QString IntentRouter::normalize(const QString& input) const {
    QString normalized = input.trimmed();
    normalized.replace(QChar(0x3000), QChar(' '));
    while (normalized.contains("  ")) {
        normalized.replace("  ", " ");
    }
    return normalized;
}