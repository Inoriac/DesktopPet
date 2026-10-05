#include "intent_router.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace {
IntentRoute routeReminder(const QString& input) {
    // Only execute an unambiguous numeric duration locally. Numbers in the
    // reminder's contents, Chinese numerals and compound durations are not defaults.
    static const QRegularExpression interval(
        QStringLiteral("每(?:隔)?\\s*([0-9]+)\\s*(分钟|小时|分)(?![钟]|\\s*[0-9一二三四五六七八九十百千万亿零〇两半又])"));
    static const QRegularExpression delay(
        QStringLiteral("(?<![0-9.＋+\\-负一二三四五六七八九十百千万亿零〇两半时钟分点至到或])([0-9]+)\\s*(分钟|小时|分)\\s*后"));
    static const QRegularExpression compoundPrefix(
        QStringLiteral("(?:小时|分钟|分|点|至|到|或|或者|[.+＋\\-负])\\s*(?:又|零)?\\s*$"));
    static const QRegularExpression delayMarker(QStringLiteral("(?:分钟|小时|分)\\s*后"));
    const auto intervalMatch = interval.match(input);
    const auto delayMatch = delay.match(input);
    const bool periodic = intervalMatch.hasMatch();
    const auto match = periodic ? intervalMatch : delayMatch;
    if (!match.hasMatch() || (periodic && delayMatch.hasMatch())
        || (periodic && (input.count(QStringLiteral("每")) != 1 || input.contains(delayMarker)))
        || (!periodic && input.contains(QStringLiteral("每")))
        || (!periodic && input.count(delayMarker) != 1)
        || (!periodic && compoundPrefix.match(input.left(match.capturedStart())).hasMatch())
        || (periodic ? interval : delay).match(input, match.capturedEnd()).hasMatch()) {
        return IntentRoute::needLlm(QStringLiteral("schedule_time_needs_interpretation"));
    }

    bool valid = false;
    qint64 minutes = match.captured(1).toLongLong(&valid);
    const qint64 multiplier = match.captured(2) == QStringLiteral("小时") ? 60 : 1;
    constexpr qint64 maxMinutes = 9007199254740991LL / 60000;
    if (!valid || minutes <= 0 || minutes > maxMinutes / multiplier) {
        return IntentRoute::needLlm(QStringLiteral("schedule_time_needs_interpretation"));
    }
    minutes *= multiplier;
    QJsonObject args;
    args["type"] = periodic ? "interval" : "once_at";
    args["title"] = periodic ? "周期提醒" : "提醒";
    args["message"] = input;
    args[periodic ? "interval_minutes" : "delay_minutes"] = minutes;
    return IntentRoute::directToolCall("schedule_create", args,
        periodic ? "schedule_interval" : "schedule_delay_minutes", 0.9);
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

    if (containsAny(normalized, {"提醒", "日程", "叫我"})
        && containsAny(normalized, {"取消", "推迟", "延后", "删除", "稍后", "不要", "别再", "不用"})) {
        return IntentRoute::needLlm("schedule_management_needs_interpretation");
    }
    if (containsAny(normalized, {"提醒列表", "查看提醒", "列出提醒", "日程列表", "查看日程"})) {
        return IntentRoute::directToolCall("schedule_list", {}, "schedule_list", 0.9);
    }
    // The reminder's contents may mention weather, battery or the current time.
    // Resolve the outer request before any keyword-only query rules.
    if (containsAny(normalized, {"提醒", "叫我"})) {
        return routeReminder(normalized);
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

    if (containsAny(normalized, {"启动lx", "打开lx", "拉起lx", "启动 lx music", "打开 lx music", "拉起 lx music"})) {
        return IntentRoute::directToolCall("lx_music_launch", {}, "lx_music_launch", 0.95);
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
