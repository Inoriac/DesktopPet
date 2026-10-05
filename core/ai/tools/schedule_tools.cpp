//
// 定时与提醒工具
//

#include "schedule_tools.h"

#include "ai/memory/memory_store.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace {
constexpr qint64 kMaxJsonMilliseconds = 9007199254740991LL;
constexpr qint64 kMaxMinutes = kMaxJsonMilliseconds / 60000;
constexpr int kListPayloadBudget = 5500; // Includes ToolResult's envelope; sanitizer defaults to 6000.

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

QJsonObject makeDurationProperty(const QString& description, qint64 minimum, qint64 maximum) {
    return {{"type", "integer"}, {"description", description},
            {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject taskSummary(const ScheduledTask& task, const QString& status = {}) {
    QJsonObject obj;
    obj["id"] = task.id;
    obj["enabled"] = task.enabled;
    obj["status"] = status.isEmpty() ? (task.enabled ? "active" : "disabled") : status;
    obj["title"] = task.title;
    obj["description"] = task.description;
    obj["trigger_type"] = task.triggerType;
    obj["message"] = task.message;
    obj["animation_state"] = task.animationState;
    obj["next_trigger_at"] = task.nextTriggerAt.isValid() ? task.nextTriggerAt.toString(Qt::ISODate) : QString();
    obj["last_triggered_at"] = task.lastTriggeredAt.isValid() ? task.lastTriggeredAt.toString(Qt::ISODate) : QString();
    const auto state = task.toJson();
    obj["trigger"] = state.value("trigger");
    obj["policy"] = state.value("policy");
    return obj;
}

QJsonObject taskListSummary(const ScheduledTask& task, const QString& status = {}) {
    auto summary = taskSummary(task, status);
    const auto displayText = [](const QString& text, int limit) {
        return text.size() <= limit ? text : text.left(limit) + QChar(0x2026);
    };
    summary["title"] = displayText(task.title, 80);
    summary["message"] = displayText(task.message, 140);
    summary["description"] = displayText(task.description, 80);
    summary["animation_state"] = displayText(task.animationState, 60);
    return summary;
}

std::optional<int> pageInteger(const QJsonObject& params, const QString& key,
                               int fallback, int maximum) {
    if (!params.contains(key)) return fallback;
    const auto value = params.value(key);
    const double number = value.toDouble(-1.0);
    if (!value.isDouble() || !std::isfinite(number) || number < 0.0
        || std::floor(number) != number || number > maximum) return std::nullopt;
    return static_cast<int>(number);
}

std::optional<qint64> snoozeMinutes(const QJsonObject& params) {
    if (!params.contains("minutes")) return 10;
    const auto value = params.value("minutes");
    if (!value.isDouble()) return std::nullopt;
    const double minutes = value.toDouble();
    constexpr double maxJsonInteger = 9007199254740991.0;
    if (!std::isfinite(minutes) || minutes <= 0.0 || std::floor(minutes) != minutes
        || minutes > maxJsonInteger || minutes > static_cast<double>(kMaxMinutes)) {
        return std::nullopt;
    }
    return static_cast<qint64>(minutes);
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

} // namespace

void connectSchedulerMemory(AgentScheduler& scheduler, MemoryStore& memoryStore) {
    scheduler.setStateSink([&memoryStore](const QJsonObject& state) {
        const auto task = ScheduledTask::fromJson(state);
        QString status = state.value("status").toString();
        if (!task.isValid() || (status != "active" && status != "disabled"
                && status != "completed" && status != "cancelled")) return false;
        if (status == "active" && !task.enabled) status = "disabled";
        MemoryEntry updated = taskShadowMemoryEntry(task, {});
        updated.status = status == "completed" || status == "disabled" ? MemoryStatus::Archived
            : status == "cancelled" ? MemoryStatus::Cancelled : MemoryStatus::Active;
        updated.value = taskSummary(task, status);
        updated.payload["next_trigger_at"] = dateTimeText(task.nextTriggerAt);
        updated.payload["last_triggered_at"] = dateTimeText(task.lastTriggeredAt);
        updated.payload["task_status"] = status;
        updated.payload["last_outcome"] = state.value("outcome");
        updated.summary = status == "completed" ? QStringLiteral("已完成提醒：%1").arg(task.title)
            : status == "cancelled" ? QStringLiteral("已取消提醒：%1").arg(task.title)
            : status == "disabled" ? QStringLiteral("已停用提醒：%1").arg(task.title)
            : QStringLiteral("待提醒「%1」：%2；下次时间 %3").arg(task.title, task.message, dateTimeText(task.nextTriggerAt));
        updated.content = updated.summary;
        updated.updatedAt = task.updatedAt;
        QString error;
        return memoryStore.synchronizeTaskShadow(updated, &error);
    });
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
    properties["delay_minutes"] = makeDurationProperty("once_at 可用：正整数分钟后提醒；不使用时省略", 1, kMaxMinutes);
    properties["interval_minutes"] = makeDurationProperty("interval 可用：每隔正整数分钟提醒；与 interval_ms 至少提供一个", 1, kMaxMinutes);
    properties["interval_ms"] = makeDurationProperty("interval 可用：每隔多少毫秒提醒，最低 60000；不使用时省略", 60000, kMaxJsonMilliseconds);
    properties["animation_state"] = makeStringProperty("可选：到点时尝试播放的动画状态，如 Talk、Happy、Sitting");
    properties["respect_quiet_hours"] = makeIntegerProperty("是否尊重勿扰时间，1=true，0=false", 1);

    schema["properties"] = properties;
    QJsonArray required;
    required.append("type");
    required.append("title");
    schema["required"] = required;
    const auto triggerBranch = [](const QString& typeName, const QStringList& alternatives) {
        QJsonArray choices;
        for (const auto& field : alternatives)
            choices.append(QJsonObject{{"required", QJsonArray{field}}});
        return QJsonObject{{"properties", QJsonObject{{"type", QJsonObject{{"enum", QJsonArray{typeName}}}}}},
                           {"anyOf", choices}};
    };
    schema["oneOf"] = QJsonArray{
        triggerBranch("once_at", {"delay_minutes", "at", "time"}),
        triggerBranch("daily_at", {"time", "at"}),
        triggerBranch("interval", {"interval_minutes", "interval_ms"})};
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

    const bool memoryRecorded = m_memoryStore && m_scheduler->synchronizeState();

    QJsonObject result;
    result["task"] = taskSummary(task);
    result["storage_path"] = m_scheduler->storagePath();
    result["memory_recorded"] = memoryRecorded;
    return ToolResult::ok(result);
}

ScheduleListTool::ScheduleListTool(AgentScheduler* scheduler)
    : AITool(
          "schedule_list",
          "分页列出当前提醒与最近已完成提醒，返回任务 id、触发时间及后续分页位置；可用已完成任务 id 稍后再提醒。",
          ToolCategory::Query)
    , m_scheduler(scheduler) {}

QJsonObject ScheduleListTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    QJsonObject properties;
    properties["include_disabled"] = makeIntegerProperty("是否包含已禁用任务，1=true，0=false", 1);
    const auto pageProperty = [](const QString& description, int fallback, int maximum) {
        auto property = makeIntegerProperty(description, fallback);
        property["minimum"] = 0;
        property["maximum"] = maximum;
        return property;
    };
    properties["offset"] = pageProperty("当前任务起始位置；续页使用 next_offset", 0, std::numeric_limits<int>::max());
    properties["limit"] = pageProperty("当前任务每页最多条数；0 表示本次不返回当前任务", 5, 100);
    properties["completed_offset"] = pageProperty("已完成记录起始位置；续页使用 completed_next_offset", 0, std::numeric_limits<int>::max());
    properties["completed_limit"] = pageProperty("已完成记录每页最多条数；0 表示本次不返回历史，按最近完成优先", 3, 100);
    schema["properties"] = properties;
    return schema;
}

ToolResult ScheduleListTool::execute(const QJsonObject& params) {
    if (!m_scheduler) {
        return ToolResult::fail("AgentScheduler 未配置");
    }
    if (!m_scheduler->storageAvailable()) {
        return ToolResult::fail("提醒存储不可用，无法确认当前提醒；请恢复存储后重试");
    }
    const auto requestedOffset = pageInteger(params, "offset", 0, std::numeric_limits<int>::max());
    const auto limit = pageInteger(params, "limit", 5, 100);
    const auto requestedCompletedOffset = pageInteger(params, "completed_offset", 0, std::numeric_limits<int>::max());
    const auto completedLimit = pageInteger(params, "completed_limit", 3, 100);
    if (!requestedOffset || !limit || !requestedCompletedOffset || !completedLimit)
        return ToolResult::fail("分页位置必须为非负整数，每页条数必须为 0 至 100 的整数");

    const bool includeDisabled = params.value("include_disabled").toInt(1) != 0;
    QList<ScheduledTask> tasks;
    for (const ScheduledTask& task : m_scheduler->tasks()) {
        if (includeDisabled || task.enabled) tasks.append(task);
    }
    auto history = m_scheduler->completedTasks();
    std::sort(history.begin(), history.end(), [](const ScheduledTask& left, const ScheduledTask& right) {
        if (left.lastTriggeredAt != right.lastTriggeredAt) return left.lastTriggeredAt > right.lastTriggeredAt;
        return left.id < right.id;
    });
    const int offset = qMin(*requestedOffset, int(tasks.size()));
    const int completedOffset = qMin(*requestedCompletedOffset, int(history.size()));
    QJsonArray items;
    QJsonArray completed;
    const auto resultPage = [&]() {
        const int next = offset + int(items.size());
        const int completedNext = completedOffset + int(completed.size());
        const bool more = next < tasks.size();
        const bool completedMore = completedNext < history.size();
        return QJsonObject{{"count", items.size()}, {"total_count", tasks.size()},
            {"offset", offset}, {"has_more", more},
            {"next_offset", more ? QJsonValue(next) : QJsonValue(QJsonValue::Null)},
            {"tasks", items}, {"recent_completed", completed},
            {"recent_completed_total", history.size()}, {"completed_offset", completedOffset},
            {"completed_has_more", completedMore},
            {"completed_next_offset", completedMore ? QJsonValue(completedNext) : QJsonValue(QJsonValue::Null)},
            {"storage_path", m_scheduler->storagePath()}};
    };
    const auto fitsBudget = [&]() {
        const auto payload = QJsonDocument(ToolResult::ok(resultPage()).toJson()).toJson(QJsonDocument::Compact);
        return QString::fromUtf8(payload).size() <= kListPayloadBudget;
    };
    // Reserve one row per requested stream before filling either page, so a
    // large current-task page cannot prevent completed-history pagination.
    if (*limit > 0 && offset < tasks.size()) items.append(taskListSummary(tasks.at(offset)));
    if (*completedLimit > 0 && completedOffset < history.size())
        completed.append(taskListSummary(history.at(completedOffset), QStringLiteral("completed")));
    if (!fitsBudget()) return ToolResult::fail("提醒标识或存储路径过长，无法在工具输出限制内返回完整分页");

    bool tasksFull = items.size() >= *limit || offset + items.size() >= tasks.size();
    bool completedFull = completed.size() >= *completedLimit || completedOffset + completed.size() >= history.size();
    while (!tasksFull || !completedFull) {
        if (!tasksFull) {
            items.append(taskListSummary(tasks.at(offset + items.size())));
            if (!fitsBudget()) { items.removeLast(); tasksFull = true; }
            else tasksFull = items.size() >= *limit || offset + items.size() >= tasks.size();
        }
        if (!completedFull) {
            completed.append(taskListSummary(history.at(completedOffset + completed.size()), QStringLiteral("completed")));
            if (!fitsBudget()) { completed.removeLast(); completedFull = true; }
            else completedFull = completed.size() >= *completedLimit || completedOffset + completed.size() >= history.size();
        }
    }
    return ToolResult::ok(resultPage());
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

    const bool memoryUpdated = m_memoryStore && m_scheduler->synchronizeState();

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
    auto minutes = makeDurationProperty("推迟多少分钟，必须为正整数，默认 10", 1, kMaxMinutes);
    minutes["default"] = 10;
    properties["minutes"] = minutes;
    schema["properties"] = properties;
    QJsonArray required;
    required.append("id");
    schema["required"] = required;
    return schema;
}

bool ScheduleSnoozeTool::validate(const QJsonObject& params) const {
    return m_scheduler && params.contains("id") && !params.value("id").toString().trimmed().isEmpty()
        && snoozeMinutes(params).has_value();
}

ToolResult ScheduleSnoozeTool::execute(const QJsonObject& params) {
    if (!m_scheduler) {
        return ToolResult::fail("AgentScheduler 未配置");
    }

    const QString id = params.value("id").toString().trimmed();
    const auto minutes = snoozeMinutes(params);
    if (!minutes.has_value()) return ToolResult::fail("推迟分钟数必须是可安全表示的正整数");
    QString error;
    if (!m_scheduler->snoozeTask(id, *minutes, &error)) {
        return ToolResult::fail(error);
    }

    const bool memoryUpdated = m_memoryStore && m_scheduler->synchronizeState();

    QJsonObject result;
    result["snoozed"] = true;
    result["id"] = id;
    result["minutes"] = *minutes;
    result["memory_updated"] = memoryUpdated;
    for (const auto& task : m_scheduler->tasks()) {
        if (task.id != id) continue;
        result["task"] = taskSummary(task);
        result["title"] = task.title;
        result["next_trigger_at"] = dateTimeText(task.nextTriggerAt);
        break;
    }
    return ToolResult::ok(result);
}
