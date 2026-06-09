//
// 定时与提醒工具
//

#include "schedule_tools.h"

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
}

ScheduleCreateTool::ScheduleCreateTool(AgentScheduler* scheduler)
    : AITool(
          "schedule_create",
          "创建桌宠定时提醒任务。支持 once_at(一次性)、daily_at(每日固定时间)、interval(固定间隔)。到点后可显示气泡和可选动画。",
          ToolCategory::Action)
    , m_scheduler(scheduler) {}

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

    QJsonObject result;
    result["task"] = taskSummary(task);
    result["storage_path"] = m_scheduler->storagePath();
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

ScheduleCancelTool::ScheduleCancelTool(AgentScheduler* scheduler)
    : AITool(
          "schedule_cancel",
          "取消指定 id 的桌宠定时提醒任务。",
          ToolCategory::Action)
    , m_scheduler(scheduler) {}

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

    QJsonObject result;
    result["cancelled"] = true;
    result["id"] = id;
    return ToolResult::ok(result);
}

ScheduleSnoozeTool::ScheduleSnoozeTool(AgentScheduler* scheduler)
    : AITool(
          "schedule_snooze",
          "将指定 id 的提醒稍后再提醒。适合用户说“稍后提醒我”。",
          ToolCategory::Action)
    , m_scheduler(scheduler) {}

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

    QJsonObject result;
    result["snoozed"] = true;
    result["id"] = id;
    result["minutes"] = minutes;
    return ToolResult::ok(result);
}
