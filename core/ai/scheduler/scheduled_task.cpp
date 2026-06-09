//
// Agent 调度任务数据模型
//

#include "scheduled_task.h"

#include <QJsonArray>
#include <QTimeZone>

namespace {
QString dateTimeToString(const QDateTime& value) {
    return value.isValid() ? value.toString(Qt::ISODate) : QString();
}

QDateTime dateTimeFromString(const QString& value) {
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed.isValid() ? parsed : QDateTime{};
}

QTime timeFromString(const QString& value) {
    QTime parsed = QTime::fromString(value, "HH:mm");
    if (!parsed.isValid()) {
        parsed = QTime::fromString(value, "HH:mm:ss");
    }
    return parsed;
}
}

QJsonObject ScheduledTask::toJson() const {
    QJsonObject obj;
    obj["id"] = id;
    obj["enabled"] = enabled;
    obj["title"] = title;
    obj["description"] = description;
    obj["source"] = source;
    obj["priority"] = priority;

    QJsonObject trigger;
    trigger["type"] = triggerType;
    if (triggerType == "once_at") {
        trigger["at"] = dateTimeToString(onceAt);
    } else if (triggerType == "daily_at") {
        trigger["time"] = dailyAt.toString("HH:mm");
    } else if (triggerType == "interval") {
        trigger["interval_ms"] = intervalMs;
    }
    obj["trigger"] = trigger;

    QJsonObject policy;
    policy["respect_quiet_hours"] = respectQuietHours;
    policy["skip_when_user_busy"] = skipWhenUserBusy;
    policy["min_gap_ms"] = minGapMs;
    policy["allow_llm"] = allowLlm;
    policy["allow_network"] = allowNetwork;
    obj["policy"] = policy;

    QJsonArray actions;
    if (!animationState.trimmed().isEmpty()) {
        QJsonObject animationAction;
        animationAction["type"] = "play_animation";
        animationAction["tool"] = "play_animation";
        QJsonObject arguments;
        arguments["state"] = animationState.trimmed();
        animationAction["arguments"] = arguments;
        actions.append(animationAction);
    }
    if (!message.trimmed().isEmpty()) {
        QJsonObject bubbleAction;
        bubbleAction["type"] = "show_bubble";
        bubbleAction["tool"] = "show_chat_bubble";
        QJsonObject arguments;
        arguments["text"] = message.trimmed();
        bubbleAction["arguments"] = arguments;
        actions.append(bubbleAction);
    }
    obj["actions"] = actions;

    obj["created_at"] = dateTimeToString(createdAt);
    obj["updated_at"] = dateTimeToString(updatedAt);
    obj["last_triggered_at"] = dateTimeToString(lastTriggeredAt);
    obj["next_trigger_at"] = dateTimeToString(nextTriggerAt);
    return obj;
}

ScheduledTask ScheduledTask::fromJson(const QJsonObject& obj) {
    ScheduledTask task;
    task.id = obj.value("id").toString();
    task.enabled = obj.value("enabled").toBool(true);
    task.title = obj.value("title").toString();
    task.description = obj.value("description").toString();
    task.source = obj.value("source").toString("user_request");
    task.priority = obj.value("priority").toInt(50);

    const QJsonObject trigger = obj.value("trigger").toObject();
    task.triggerType = trigger.value("type").toString("once_at");
    task.onceAt = dateTimeFromString(trigger.value("at").toString());
    task.dailyAt = timeFromString(trigger.value("time").toString());
    task.intervalMs = trigger.value("interval_ms").toInt(0);

    const QJsonObject policy = obj.value("policy").toObject();
    task.respectQuietHours = policy.value("respect_quiet_hours").toBool(true);
    task.skipWhenUserBusy = policy.value("skip_when_user_busy").toBool(false);
    task.minGapMs = policy.value("min_gap_ms").toInt(0);
    task.allowLlm = policy.value("allow_llm").toBool(false);
    task.allowNetwork = policy.value("allow_network").toBool(false);

    const QJsonArray actions = obj.value("actions").toArray();
    for (const QJsonValue& value : actions) {
        const QJsonObject action = value.toObject();
        const QString tool = action.value("tool").toString();
        const QJsonObject arguments = action.value("arguments").toObject();
        if (tool == "show_chat_bubble") {
            task.message = arguments.value("text").toString();
        } else if (tool == "play_animation") {
            task.animationState = arguments.value("state").toString();
        }
    }

    task.createdAt = dateTimeFromString(obj.value("created_at").toString());
    task.updatedAt = dateTimeFromString(obj.value("updated_at").toString());
    task.lastTriggeredAt = dateTimeFromString(obj.value("last_triggered_at").toString());
    task.nextTriggerAt = dateTimeFromString(obj.value("next_trigger_at").toString());
    return task;
}

bool ScheduledTask::isValid(QString* errorMessage) const {
    if (id.trimmed().isEmpty()) {
        if (errorMessage) *errorMessage = "任务 id 不能为空";
        return false;
    }
    if (title.trimmed().isEmpty()) {
        if (errorMessage) *errorMessage = "任务标题不能为空";
        return false;
    }
    if (message.trimmed().isEmpty() && animationState.trimmed().isEmpty()) {
        if (errorMessage) *errorMessage = "任务至少需要一条气泡消息或动画动作";
        return false;
    }
    if (triggerType == "once_at") {
        if (!onceAt.isValid()) {
            if (errorMessage) *errorMessage = "一次性任务缺少有效时间";
            return false;
        }
    } else if (triggerType == "daily_at") {
        if (!dailyAt.isValid()) {
            if (errorMessage) *errorMessage = "每日任务缺少有效时间";
            return false;
        }
    } else if (triggerType == "interval") {
        if (intervalMs < 60000) {
            if (errorMessage) *errorMessage = "固定间隔任务的间隔不能小于 60 秒";
            return false;
        }
    } else {
        if (errorMessage) *errorMessage = QString("不支持的触发类型: %1").arg(triggerType);
        return false;
    }
    return true;
}

void ScheduledTask::refreshNextTrigger(const QDateTime& now) {
    if (triggerType == "once_at") {
        nextTriggerAt = onceAt;
        return;
    }

    if (triggerType == "daily_at") {
        QDateTime next(now.date(), dailyAt, now.timeZone());
        if (next <= now) {
            next = next.addDays(1);
        }
        nextTriggerAt = next;
        return;
    }

    if (triggerType == "interval") {
        const QDateTime base = lastTriggeredAt.isValid() ? lastTriggeredAt : now;
        nextTriggerAt = base.addMSecs(intervalMs);
        if (nextTriggerAt <= now) {
            nextTriggerAt = now.addMSecs(intervalMs);
        }
    }
}

bool ScheduledTask::shouldTrigger(const QDateTime& now) const {
    return enabled && nextTriggerAt.isValid() && nextTriggerAt <= now;
}
