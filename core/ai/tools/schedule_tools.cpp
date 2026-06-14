//
// 定时与提醒工具
//

#include "schedule_tools.h"

#include "ai/memory/memory_store.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>

namespace {
QJsonObject makeStringProperty(const QString& description) {
    QJsonObject obj;
    obj["type"] = "string";
    obj["description"] = description;
    return obj;
}

QJsonObject makeIntegerProperty(const QString& description, int defaultValue = 0) {
    QJsonObject obj;
    obj["type"] = "integer";
    obj["description"] = description;
    obj["default"] = defaultValue;
    return obj;
}

QJsonObject taskSummary(const ScheduledTask& task) {
    QJsonObject obj;
    obj["id"] = task.id;
    obj["enabled"] = task.enabled;
    obj["title"] = task.title;
    obj["description"] = task.description;
    obj["trigger_type"] = task.triggerType;
    obj["message"] = task.message;
    obj["animation_state"] = task.animationState;
    obj["next_trigger_at"] = task.nextTriggerAt.isValid() ? task.nextTriggerAt.toString(Qt::ISODate) : QString();
    obj["last_triggered_at"] = task.lastTriggeredAt.isValid() ? task.lastTriggeredAt.toString(Qt::ISODate) : QString();
    return obj;
}

QString dateTimeText(const QDateTime& value) {
    return value.isValid() ? value.toUTC().toString(Qt::ISODate) : QString();
}

MemoryEntry taskShadowMemoryEntry(const ScheduledTask& task, const QJsonObject& createParams) {
    const QString message = task.message.trimmed();
    const QString title = task.title.trimmed();
    const QString summary = message.isEmpty()
        ? QStringLiteral("提醒任务：%1").arg(title)
        : QStringLiteral("提醒任务「%1」：%2").arg(title, message);

    MemoryEntry entry;
    entry.type = MemoryType::TaskShadow;
    entry.status = MemoryStatus::Active;
    entry.privacyLevel = PrivacyLevel::Personal;
    entry.key = QStringLiteral("schedule:%1").arg(task.id);
    entry.value = taskSummary(task);
    entry.summary = summary;
    entry.content = summary;
    entry.tags = {QStringLiteral("schedule"), QStringLiteral("reminder"), QStringLiteral("task_shadow"), task.triggerType};
    entry.scope = QStringLiteral("reminder");
    entry.source = QStringLiteral("schedule_create");
    entry.importance = task.priority >= 80 ? 0.8 : 0.6;
    entry.strength = entry.importance;
    entry.confidence = 1.0;
    if (!message.isEmpty()) {
        entry.evidence.append(message);
    }
    if (!task.description.trimmed().isEmpty()) {
        entry.evidence.append(task.description.trimmed());
    }

    QJsonObject payload;
    payload["linked_task_id"] = task.id;
    payload["title"] = task.title;
    payload["description"] = task.description;
    payload["message"] = task.message;
    payload["trigger_type"] = task.triggerType;
    payload["next_trigger_at"] = dateTimeText(task.nextTriggerAt);
    payload["created_at"] = dateTimeText(task.createdAt);
    payload["animation_state"] = task.animationState;
    payload["scheduler_source"] = task.source;
    payload["create_params"] = createParams;
    entry.payload = payload;
    return entry;
}

bool saveMemoryStore(MemoryStore* memoryStore) {
    if (!memoryStore) {
        return false;
    }

    QString error;
    if (!memoryStore->save(&error)) {
        qWarning() << "[ScheduleTools] Failed to save memory store:" << error;
        return false;
    }
    return true;
}
}

ScheduleCreateTool::ScheduleCreateTool(AgentScheduler* scheduler, MemoryStore* memoryStore)
    : AITool(
          "schedule_create",
          "创建桌宠定时提醒任务。支持 once_at(一次性)、daily_at(每日固定时间)、interval(固定间隔)。到点后可显示气泡和可选动画。",
          ToolCategory::Action)
    , m_scheduler(scheduler)
    , m_memoryStore(memoryStore) {}

QJsonObject ScheduleCreateTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";

    QJsonObject properties;
    QJsonObject type = makeStringProperty("触发类型：once_at、daily_at 或 interval");
    QJsonArray typeEnum;
    typeEnum.append("once_at");
    typeEnum.append("daily_at");
    typeEnum.append("interval");
    type["enum"] = typeEnum;
    properties["type"] = type;
    properties["title"] = makeStringProperty("任务标题，如：提醒喝水");
    properties["message"] = makeStringProperty("到点时桌宠气泡显示的文本");
    properties["at"] = makeStringProperty("once_at 可用：ISO 时间、yyyy-MM-dd HH:mm 或 HH:mm；daily_at 可用：HH:mm");
    properties["time"] = makeStringProperty("daily_at 可用：每天触发时间 HH:mm；once_at 也可传 HH:mm");
    properties["delay_minutes"] = makeIntegerProperty("once_at 可用：多少分钟后提醒", 0);
    properties["interval_minutes"] = makeIntegerProperty("interval 可用：每隔多少分钟提醒", 0);
    properties["interval_ms"] = makeIntegerProperty("interval 可用：每隔多少毫秒提醒，最低 60000", 0);
    properties["animation_state"] = makeStringProperty("可选：到点时尝试播放的动画状态，如 Talk、Happy、Sitting");
    properties["respect_quiet_hours"] = makeIntegerProperty("是否尊重勿扰时间，1=true，0=false", 1);

    schema["properties"] = properties;
    QJsonArray required;
    required.append("type");
    required.append("title");
    schema["required"] = required;
    return schema;
}

bool ScheduleCreateTool::validate(const QJsonObject& params) const {
    if (!m_scheduler) {
        return false;
    }
    if (!params.contains("type") || !params.contains("title")) {
        return false;
    }
    const QString type = params.value("type").toString();
    return type == "once_at" || type == "daily_at" || type == "interval";
}

ToolResult ScheduleCreateTool::execute(const QJsonObject& params) {
    if (!m_scheduler) {
        return ToolResult::fail("AgentScheduler 未配置");
    }

    QString error;
    const ScheduledTask task = m_scheduler->createTask(params, &error);
    if (task.id.isEmpty()) {
        return ToolResult::fail(error.isEmpty() ? QString("创建任务失败") : error);
    }

    bool memoryRecorded = false;
    if (m_memoryStore) {
        m_memoryStore->addEntry(taskShadowMemoryEntry(task, params));
        memoryRecorded = saveMemoryStore(m_memoryStore);
    }

    QJsonObject result;
    result["task"] = taskSummary(task);
    result["storage_path"] = m_scheduler->storagePath();
    result["memory_recorded"] = memoryRecorded;
    return ToolResult::ok(result);
}

ScheduleListTool::ScheduleListTool(AgentScheduler* scheduler)
    : AITool(
          "schedule_list",
          "列出当前桌宠定时提醒任务。用于查看任务 id、标题、触发类型和下次触发时间。",
          ToolCategory::Query)
    , m_scheduler(scheduler) {}

QJsonObject ScheduleListTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    QJsonObject properties;
    properties["include_disabled"] = makeIntegerProperty("是否包含已禁用任务，1=true，0=false", 1);
    schema["properties"] = properties;
    return schema;
}

ToolResult ScheduleListTool::execute(const QJsonObject& params) {
    if (!m_scheduler) {
        return ToolResult::fail("AgentScheduler 未配置");
    }

    const bool includeDisabled = params.value("include_disabled").toInt(1) != 0;
    QJsonArray items;
    for (const ScheduledTask& task : m_scheduler->tasks()) {
        if (!includeDisabled && !task.enabled) {
            continue;
        }
        items.append(taskSummary(task));
    }

    QJsonObject result;
    result["count"] = items.size();
    result["tasks"] = items;
    result["storage_path"] = m_scheduler->storagePath();
    return ToolResult::ok(result);
}

ScheduleCancelTool::ScheduleCancelTool(AgentScheduler* scheduler, MemoryStore* memoryStore)
    : AITool(
          "schedule_cancel",
          "取消指定 id 的桌宠定时提醒任务。",
          ToolCategory::Action)
    , m_scheduler(scheduler)
    , m_memoryStore(memoryStore) {}

QJsonObject ScheduleCancelTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    QJsonObject properties;
    properties["id"] = makeStringProperty("要取消的任务 id，可先用 schedule_list 查看");
    schema["properties"] = properties;
    QJsonArray required;
    required.append("id");
    schema["required"] = required;
    return schema;
}

bool ScheduleCancelTool::validate(const QJsonObject& params) const {
    return m_scheduler && params.contains("id") && !params.value("id").toString().trimmed().isEmpty();
}

ToolResult ScheduleCancelTool::execute(const QJsonObject& params) {
    if (!m_scheduler) {
        return ToolResult::fail("AgentScheduler 未配置");
    }

    const QString id = params.value("id").toString().trimmed();
    QString error;
    if (!m_scheduler->cancelTask(id, &error)) {
        return ToolResult::fail(error);
    }

    QJsonObject memoryPatch;
    memoryPatch["cancelled_at"] = dateTimeText(QDateTime::currentDateTimeUtc());
    memoryPatch["last_user_action"] = "cancelled";
    const bool memoryUpdated = m_memoryStore
        && m_memoryStore->updateTaskShadowStatus(id, MemoryStatus::Cancelled, memoryPatch)
        && saveMemoryStore(m_memoryStore);

    QJsonObject result;
    result["cancelled"] = true;
    result["id"] = id;
    result["memory_updated"] = memoryUpdated;
    return ToolResult::ok(result);
}

ScheduleSnoozeTool::ScheduleSnoozeTool(AgentScheduler* scheduler, MemoryStore* memoryStore)
    : AITool(
          "schedule_snooze",
          "将指定 id 的提醒稍后再提醒。适合用户说“稍后提醒我”。",
          ToolCategory::Action)
    , m_scheduler(scheduler)
    , m_memoryStore(memoryStore) {}

QJsonObject ScheduleSnoozeTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    QJsonObject properties;
    properties["id"] = makeStringProperty("要推迟的任务 id，可先用 schedule_list 查看");
    properties["minutes"] = makeIntegerProperty("推迟多少分钟，默认 10", 10);
    schema["properties"] = properties;
    QJsonArray required;
    required.append("id");
    schema["required"] = required;
    return schema;
}

bool ScheduleSnoozeTool::validate(const QJsonObject& params) const {
    return m_scheduler && params.contains("id") && !params.value("id").toString().trimmed().isEmpty();
}

ToolResult ScheduleSnoozeTool::execute(const QJsonObject& params) {
    if (!m_scheduler) {
        return ToolResult::fail("AgentScheduler 未配置");
    }

    const QString id = params.value("id").toString().trimmed();
    const int minutes = qMax(1, params.value("minutes").toInt(10));
    QString error;
    if (!m_scheduler->snoozeTask(id, minutes, &error)) {
        return ToolResult::fail(error);
    }

    QJsonObject memoryPatch;
    memoryPatch["last_snoozed_at"] = dateTimeText(QDateTime::currentDateTimeUtc());
    memoryPatch["last_snooze_minutes"] = minutes;
    memoryPatch["last_user_action"] = "snoozed";
    const bool memoryUpdated = m_memoryStore
        && m_memoryStore->updateTaskShadowStatus(id, MemoryStatus::Active, memoryPatch)
        && saveMemoryStore(m_memoryStore);

    QJsonObject result;
    result["snoozed"] = true;
    result["id"] = id;
    result["minutes"] = minutes;
    result["memory_updated"] = memoryUpdated;
    return ToolResult::ok(result);
}
