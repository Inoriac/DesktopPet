#include "intent_router.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace {
int firstCapturedInt(const QString& input, int fallback = 0) {
    const QRegularExpression re("(\\d+)");
    const QRegularExpressionMatch match = re.match(input);
    return match.hasMatch() ? match.captured(1).toInt() : fallback;
}

QString extractLocationFromLifeAssistantQuery(const QString& normalizedInput) {
    QString candidate = normalizedInput;
    candidate.replace(QRegularExpression("[\\s，,。.!！?？；;：:、]+"), "");

    const QStringList removablePhrases = {
        "天气预报", "天气怎么样", "天气如何", "会不会下雨", "会下雨吗", "下不下雨", "多少度", "几度",
        "今日简报", "每日简报", "今天简报", "生成简报", "查看简报", "早报",
        "查看一下", "查询一下", "查一下", "看一下", "帮我看看", "帮我查查", "帮我查询",
        "帮我", "帮忙", "请问", "我想知道", "我要", "查询", "查看", "查查", "看看", "看下",
        "你好", "hello", "hi", "嗨", "在吗", "在不在",
        "现在", "当前", "今天", "今日", "明天", "明日", "最近", "本地", "当地",
        "天气", "气温", "温度", "下雨", "有雨", "降雨", "预报", "简报",
        "会不会", "是不是", "怎么样", "如何", "怎样", "多少", "什么", "会", "有", "的", "了", "吗", "呢", "呀", "吧"
    };

    for (const QString& phrase : removablePhrases) {
        candidate.replace(phrase, "", Qt::CaseInsensitive);
    }

    candidate = candidate.trimmed();
    return candidate;
}

bool isPureGreeting(const QString& normalizedInput) {
    QString compact = normalizedInput.trimmed().toLower();
    compact.replace(QRegularExpression("[\\s，,。.!！?？；;：:、~～]+"), "");

    static const QSet<QString> greetings = {
        QStringLiteral("你好"),
        QStringLiteral("你好呀"),
        QStringLiteral("你好啊"),
        QStringLiteral("您好"),
        QStringLiteral("嗨"),
        QStringLiteral("嗨嗨"),
        QStringLiteral("hi"),
        QStringLiteral("hello"),
        QStringLiteral("hey"),
        QStringLiteral("在吗"),
        QStringLiteral("在不在")
    };

    return greetings.contains(compact);
}
}

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

    if (containsAny(normalized, {"我空闲了吗", "我离开多久", "空闲状态", "多久没操作"})) {
        return IntentRoute::directToolCall("get_user_idle_state", {}, "user_idle_state", 0.9);
    }

    if (containsAny(normalized, {"电量", "电池", "低电量", "充电"})) {
        return IntentRoute::directToolCall("get_battery_status", {}, "battery_status", 0.9);
    }

    if (containsAny(normalized, {"网络状态", "网络正常", "联网了吗", "能不能联网"})) {
        return IntentRoute::directToolCall("get_network_status", {}, "network_status", 0.9);
    }

    if (containsAny(normalized, {"天气", "下雨", "有雨", "降雨", "气温", "温度"})) {
        const QString location = extractLocationFromLifeAssistantQuery(normalized);
        if (location.isEmpty()) {
            return IntentRoute::needClarification("你想查询哪个城市或地点的天气？", "weather_missing_location");
        }
        QJsonObject args;
        args["location"] = location;
        return IntentRoute::directToolCall("weather_query", args, "weather_query", 0.82);
    }

    if (containsAny(normalized, {"今日简报", "每日简报", "今天简报", "早报"})) {
        QJsonObject args;
        args["location"] = extractLocationFromLifeAssistantQuery(normalized);
        return IntentRoute::directToolCall("daily_briefing", args, "daily_briefing", 0.86);
    }

    if (containsAny(normalized, {"今天节日", "节假日", "今天放假吗", "是不是周末"})) {
        return IntentRoute::directToolCall("holiday_query", {}, "holiday_query", 0.84);
    }

    if (containsAny(normalized, {"提醒列表", "查看提醒", "列出提醒", "日程列表", "查看日程"})) {
        return IntentRoute::directToolCall("schedule_list", {}, "schedule_list", 0.9);
    }

    if (containsAny(normalized, {"安静一点", "别打扰", "勿扰", "专注模式"})) {
        QJsonObject args;
        args["mode"] = "focus";
        return IntentRoute::directToolCall("set_proactive_mode", args, "set_focus_mode", 0.9);
    }

    if (containsAny(normalized, {"活泼一点", "多陪我", "主动一点"})) {
        QJsonObject args;
        args["mode"] = "lively";
        return IntentRoute::directToolCall("set_proactive_mode", args, "set_lively_mode", 0.85);
    }

    if (containsAny(normalized, {"普通模式", "正常模式", "恢复主动"})) {
        QJsonObject args;
        args["mode"] = "normal";
        return IntentRoute::directToolCall("set_proactive_mode", args, "set_normal_mode", 0.85);
    }

    if (containsAny(normalized, {"提醒我", "叫我", "提醒一下"}) && containsAny(normalized, {"分钟后", "分后"})) {
        const int minutes = firstCapturedInt(normalized, 10);
        QJsonObject args;
        args["type"] = "once_at";
        args["title"] = "提醒";
        args["message"] = normalized;
        args["delay_minutes"] = minutes;
        return IntentRoute::directToolCall("schedule_create", args, "schedule_delay_minutes", 0.8);
    }

    if (containsAny(normalized, {"每隔", "每"}) && containsAny(normalized, {"提醒我", "提醒一下"}) && containsAny(normalized, {"分钟", "小时"})) {
        int minutes = firstCapturedInt(normalized, 60);
        if (normalized.contains("小时")) {
            minutes *= 60;
        }
        QJsonObject args;
        args["type"] = "interval";
        args["title"] = "周期提醒";
        args["message"] = normalized;
        args["interval_minutes"] = minutes;
        return IntentRoute::directToolCall("schedule_create", args, "schedule_interval", 0.75);
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
        return IntentRoute::directToolCall("lx_music_skip_next", {}, "lx_music_skip_next", 0.9);
    }

    if (isPureGreeting(normalized)) {
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