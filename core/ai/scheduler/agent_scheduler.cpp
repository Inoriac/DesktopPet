//
// Agent 主动调度器
//

#include "agent_scheduler.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTime>
#include <QTimeZone>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace {
constexpr qint64 kMaxJsonInteger = 9007199254740991LL;
constexpr qint64 kMsPerMinute = 60000;
constexpr int kCompletedTaskLimit = 100;

bool readInteger(const QJsonValue& value, const QString& name, qint64 minimum,
                 qint64 maximum, qint64* result, QString* errorMessage) {
    const double number = value.toDouble(-1.0);
    if (!value.isDouble() || !std::isfinite(number) || std::trunc(number) != number
        || number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) {
        if (errorMessage) *errorMessage = QStringLiteral("%1 必须是 %2 至 %3 范围内的整数")
            .arg(name).arg(minimum).arg(maximum);
        return false;
    }
    *result = static_cast<qint64>(number);
    return true;
}

QTime parseTime(const QString& value, const QTime& fallback = {}) {
    QTime parsed = QTime::fromString(value.trimmed(), "HH:mm");
    if (!parsed.isValid()) {
        parsed = QTime::fromString(value.trimmed(), "HH:mm:ss");
    }
    return parsed.isValid() ? parsed : fallback;
}

QDateTime parseDateTime(const QString& value, const QDateTime& now) {
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    QDateTime parsed = QDateTime::fromString(trimmed, Qt::ISODate);
    if (parsed.isValid()) {
        return parsed;
    }

    parsed = QDateTime::fromString(trimmed, "yyyy-MM-dd HH:mm:ss");
    if (parsed.isValid()) {
        return parsed;
    }

    parsed = QDateTime::fromString(trimmed, "yyyy-MM-dd HH:mm");
    if (parsed.isValid()) {
        return parsed;
    }

    const QTime time = parseTime(trimmed);
    if (time.isValid()) {
        QDateTime candidate(now.date(), time, now.timeZone());
        if (candidate <= now) {
            candidate = candidate.addDays(1);
        }
        return candidate;
    }

    return {};
}

bool minutesToMs(qint64 minutes, qint64* milliseconds) {
    if (minutes <= 0 || minutes > kMaxJsonInteger / kMsPerMinute) return false;
    *milliseconds = minutes * kMsPerMinute;
    return true;
}
}

AgentScheduler::AgentScheduler(QObject* parent)
    : QObject(parent)
    , m_storagePath(defaultStoragePath()) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &AgentScheduler::checkDueTasks);
}

AgentScheduler::~AgentScheduler() { stop(); }

bool AgentScheduler::acquireStorage() const {
    if (!m_storageReady) return false;
    if (m_storageLock) return m_storageLock->isLocked();
    if (!QDir().mkpath(QFileInfo(m_storagePath).absolutePath())) return false;
    auto lock = std::make_unique<QLockFile>(m_storagePath + QStringLiteral(".lock"));
    lock->setStaleLockTime(0); // An idle live owner must never lose its tasks to another process.
    if (!lock->tryLock(0)) return false;
    m_storageLock = std::move(lock);
    return true;
}

bool AgentScheduler::storageAvailable() const {
    return m_storageReady && acquireStorage();
}

void AgentScheduler::setUserBusyProvider(std::function<bool()> provider) {
    m_userBusyProvider = std::move(provider);
}

bool AgentScheduler::configureProfileStorage(const QString& appDataRoot, const QString& profileId,
                                              const QStringList& registeredIds, QString* error) {
    if (!registeredIds.contains(profileId) || QUuid(profileId).isNull()) {
        m_storageReady = false;
        if (error) *error = QStringLiteral("提醒角色标识无效");
        return false;
    }
    setStoragePath(QDir(appDataRoot).filePath(
        QStringLiteral("profiles/%1/scheduled_tasks.json").arg(profileId)));
    if (!acquireStorage()) {
        m_storageReady = false;
        if (error) *error = QStringLiteral("该角色的提醒已由另一个实例管理，或存储不可写");
        return false;
    }
    const QString legacy = defaultStoragePath();
    if (!QFile::exists(m_storagePath) && QFile::exists(legacy) && registeredIds.size() == 1) {
        if (!QFile::copy(legacy, m_storagePath)) {
            m_storageReady = false;
            if (error) *error = QStringLiteral("旧提醒迁移失败，原文件已保留");
            return false;
        }
    }
    if (!load()) {
        if (error) *error = QStringLiteral("提醒文件无法读取，已停止调度以保留原数据");
        return false;
    }
    return true;
}

void AgentScheduler::setStateSink(StateSink sink) {
    m_stateSink = std::move(sink);
    // Reconcile legacy tasks too; queued terminal states take precedence.
    for (const auto& task : m_tasks) {
        if (!m_pendingStates.contains(task.id))
            queueState(task, task.enabled ? QStringLiteral("active") : QStringLiteral("disabled"));
    }
    for (const auto& task : m_completedTasks) {
        if (!m_pendingStates.contains(task.id)) queueState(task, QStringLiteral("completed"));
    }
    if (save()) synchronizeState();
}

void AgentScheduler::queueState(const ScheduledTask& task, const QString& status,
                                 const QString& outcome) {
    QJsonObject state = task.toJson();
    state[QStringLiteral("status")] = status;
    state[QStringLiteral("outcome")] = outcome;
    m_pendingStates.insert(task.id, state);
}

bool AgentScheduler::synchronizeState() {
    if (m_pendingStates.isEmpty()) return true;
    if (!m_stateSink || !m_storageReady || !acquireStorage()) return false;
    const auto original = m_pendingStates;
    for (auto it = original.cbegin(); it != original.cend(); ++it) {
        if (m_stateSink(it.value())) m_pendingStates.remove(it.key());
    }
    if (!save()) { m_pendingStates = original; return false; }
    return m_pendingStates.isEmpty();
}

void AgentScheduler::setToolRegistry(ToolRegistry* registry) {
    m_toolRegistry = registry;
    m_toolRuntime.setToolRegistry(registry);
}

void AgentScheduler::setStoragePath(const QString& storagePath) {
    if (!m_running && !storagePath.trimmed().isEmpty()) {
        m_storageLock.reset();
        m_storageReady = true;
        m_storagePath = storagePath;
    }
}

bool AgentScheduler::load() {
    if (!acquireStorage()) return false;
    const auto fail = [this]() {
        stop();
        m_storageReady = false;
        return false;
    };
    QFile file(m_storagePath);
    if (!file.exists()) {
        m_tasks.clear();
        m_completedTasks.clear();
        m_pendingStates.clear();
        scheduleNextTick();
        return true;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return fail();

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return fail();
    const QJsonObject root = doc.object();
    const auto version = root.value("version");
    if (!version.isUndefined() && version != QJsonValue(1) && version != QJsonValue(2))
        return fail();
    if (!root.value("tasks").isArray()
        || (root.contains("pending_states") && !root.value("pending_states").isArray())
        || (root.contains("completed_tasks") && !root.value("completed_tasks").isArray()))
        return fail();

    // Parse into temporary containers: a partially valid file must never be
    // rewritten with the rows that happened to survive deserialization.
    const auto readTask = [](const QJsonValue& value, ScheduledTask& task) {
        if (!value.isObject()) return false;
        const auto object = value.toObject();
        for (const auto& field : {"source", "description"}) {
            if (object.contains(field) && !object.value(field).isString()) return false;
        }
        if (object.contains("priority")) {
            const auto priority = object.value("priority");
            if (!priority.isDouble() || priority.toDouble() != priority.toInt()) return false;
        }
        if (!object.value("trigger").isObject() || !object.value("actions").isArray()
            || (object.contains("enabled") && !object.value("enabled").isBool())
            || (object.contains("policy") && !object.value("policy").isObject())) return false;
        const auto trigger = object.value("trigger").toObject();
        if (!trigger.value("type").isString()) return false;
        QSet<QString> actionTools;
        for (const auto& actionValue : object.value("actions").toArray()) {
            if (!actionValue.isObject()) return false;
            const auto action = actionValue.toObject();
            const QString tool = action.value("tool").toString();
            if ((tool != "show_chat_bubble" && tool != "play_animation")
                || actionTools.contains(tool) || !action.value("arguments").isObject()) return false;
            actionTools.insert(tool);
            const auto argument = action.value("arguments").toObject().value(
                tool == "show_chat_bubble" ? "text" : "state");
            if (!argument.isString() || argument.toString().trimmed().isEmpty()) return false;
        }
        const auto policy = object.value("policy").toObject();
        for (const auto& field : {"respect_quiet_hours", "skip_when_user_busy", "allow_llm", "allow_network"}) {
            if (policy.contains(field) && !policy.value(field).isBool()) return false;
        }
        for (const auto& field : {"created_at", "updated_at", "last_triggered_at", "next_trigger_at"}) {
            if (!object.contains(field)) continue;
            const auto timestamp = object.value(field);
            if (!timestamp.isString()
                || (!timestamp.toString().isEmpty()
                    && !QDateTime::fromString(timestamp.toString(), Qt::ISODate).isValid())) return false;
        }
        task = ScheduledTask::fromJson(object);
        return task.isValid();
    };

    QMap<QString, QJsonObject> pendingStates;
    for (const auto& value : root.value("pending_states").toArray()) {
        ScheduledTask task;
        if (!readTask(value, task)) return fail();
        const auto state = value.toObject();
        const QString status = state.value("status").toString();
        if (pendingStates.contains(task.id)
            || (status != "active" && status != "disabled"
                && status != "completed" && status != "cancelled")) return fail();
        pendingStates.insert(task.id, state);
    }
    QList<ScheduledTask> tasks;
    QList<ScheduledTask> completedTasks;
    QSet<QString> ids;
    const QDateTime now = QDateTime::currentDateTime();
    for (const QJsonValue& value : root.value("tasks").toArray()) {
        ScheduledTask task;
        if (!readTask(value, task) || ids.contains(task.id)) return fail();
        ids.insert(task.id);
        if (!task.nextTriggerAt.isValid()) {
            task.refreshNextTrigger(now);
        }
        if (!task.nextTriggerAt.isValid() || !task.isValid()) return fail();
        tasks.append(task);
    }
    for (const auto& value : root.value("completed_tasks").toArray()) {
        ScheduledTask task;
        if (!readTask(value, task) || ids.contains(task.id) || task.triggerType != "once_at"
            || !task.lastTriggeredAt.isValid() || task.enabled || task.nextTriggerAt.isValid()) return fail();
        ids.insert(task.id);
        completedTasks.append(task);
    }
    m_tasks = std::move(tasks);
    m_completedTasks = std::move(completedTasks);
    m_pendingStates = std::move(pendingStates);
    scheduleNextTick();
    return true;
}

bool AgentScheduler::save() const {
    if (!acquireStorage()) return false;
    if (!QDir().mkpath(QFileInfo(m_storagePath).absolutePath())) {
        return false;
    }

    QJsonArray items;
    for (const ScheduledTask& task : m_tasks) {
        if (!task.isValid() || !task.nextTriggerAt.isValid()) return false;
        items.append(task.toJson());
    }

    QJsonObject root;
    root["version"] = 2;
    QJsonArray pending;
    for (const auto& state : m_pendingStates) pending.append(state);
    root["pending_states"] = pending;
    root["tasks"] = items;
    QJsonArray completed;
    for (const auto& task : m_completedTasks) {
        if (!task.isValid()) return false;
        completed.append(task.toJson());
    }
    root["completed_tasks"] = completed;

    QSaveFile file(m_storagePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

void AgentScheduler::start() {
    if (m_running) {
        return;
    }
    if (!acquireStorage()) return;
    m_running = true;
    synchronizeState();
    scheduleNextTick();
}

void AgentScheduler::stop() {
    m_running = false;
    m_timer.stop();
}

ScheduledTask AgentScheduler::createTask(const QJsonObject& params, QString* errorMessage) {
    if (!acquireStorage()) {
        if (errorMessage) *errorMessage = QStringLiteral("该角色提醒不可用或由另一个实例管理");
        return {};
    }
    const QDateTime now = QDateTime::currentDateTime();

    ScheduledTask task;
    task.id = params.value("id").toString();
    if (task.id.trimmed().isEmpty()) {
        task.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    task.enabled = true;
    task.title = params.value("title").toString("提醒").trimmed();
    task.description = params.value("description").toString();
    task.source = params.value("source").toString("user_request");
    task.priority = params.value("priority").toInt(50);
    task.message = params.value("message").toString();
    if (task.message.trimmed().isEmpty()) {
        task.message = task.title;
    }
    task.animationState = params.value("animation_state").toString();

    task.triggerType = params.value("type").toString(params.value("trigger_type").toString("once_at")).trimmed();
    if (task.triggerType == "once" || task.triggerType == "onceAt") {
        task.triggerType = "once_at";
    } else if (task.triggerType == "daily" || task.triggerType == "dailyAt") {
        task.triggerType = "daily_at";
    }

    if (task.triggerType == "once_at") {
        if (params.contains("delay_minutes")) {
            qint64 delayMinutes = 0, delayMs = 0;
            if (!readInteger(params.value("delay_minutes"), QStringLiteral("delay_minutes"),
                             1, kMaxJsonInteger / kMsPerMinute, &delayMinutes, errorMessage)
                || !minutesToMs(delayMinutes, &delayMs)) return {};
            task.onceAt = now.addMSecs(delayMs);
        } else {
            task.onceAt = parseDateTime(params.value("at").toString(params.value("time").toString()), now);
        }
    } else if (task.triggerType == "daily_at") {
        task.dailyAt = parseTime(params.value("time").toString(params.value("at").toString()));
    } else if (task.triggerType == "interval") {
        qint64 intervalMs = 0;
        if (params.contains("interval_ms")) {
            if (!readInteger(params.value("interval_ms"), QStringLiteral("interval_ms"),
                             kMsPerMinute, kMaxJsonInteger, &intervalMs, errorMessage)) return {};
        } else {
            qint64 intervalMinutes = 0;
            if (!readInteger(params.value("interval_minutes"), QStringLiteral("interval_minutes"),
                             1, kMaxJsonInteger / kMsPerMinute, &intervalMinutes, errorMessage)
                || !minutesToMs(intervalMinutes, &intervalMs)) return {};
        }
        task.intervalMs = intervalMs;
    }

    const auto quiet = params.value("respect_quiet_hours");
    task.respectQuietHours = quiet.isDouble() ? quiet.toInt() != 0 : quiet.toBool(true);
    task.skipWhenUserBusy = params.value("skip_when_user_busy").toBool(false);
    if (params.contains("min_gap_ms")
        && !readInteger(params.value("min_gap_ms"), QStringLiteral("min_gap_ms"),
                        0, kMaxJsonInteger, &task.minGapMs, errorMessage)) return {};
    task.allowLlm = false;
    task.allowNetwork = false;
    task.createdAt = now;
    task.updatedAt = now;
    task.refreshNextTrigger(now);

    QString error;
    if (!task.isValid(&error) || !task.nextTriggerAt.isValid()) {
        if (errorMessage) {
            *errorMessage = error.isEmpty() ? QStringLiteral("任务缺少有效的下次触发时间") : error;
        }
        return {};
    }

    for (const auto& existing : m_tasks) {
        if (existing.id == task.id) {
            if (errorMessage) *errorMessage = QStringLiteral("提醒 id 已存在");
            return {};
        }
    }
    for (const auto& completed : m_completedTasks) {
        if (completed.id == task.id) {
            if (errorMessage) *errorMessage = QStringLiteral("提醒 id 已存在于已完成记录，可使用稍后提醒");
            return {};
        }
    }
    const auto originalStates = m_pendingStates;
    m_tasks.append(task);
    queueState(task, QStringLiteral("active"), QStringLiteral("created"));
    if (!save()) {
        m_pendingStates = originalStates;
        m_tasks.removeLast();
        if (errorMessage) {
            *errorMessage = QString("无法持久化调度任务: %1").arg(m_storagePath);
        }
        return {};
    }
    synchronizeState();
    scheduleNextTick();
    emit taskChanged();
    return task;
}

bool AgentScheduler::cancelTask(const QString& id, QString* errorMessage) {
    if (!acquireStorage()) {
        if (errorMessage) *errorMessage = QStringLiteral("提醒存储不可用");
        return false;
    }
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].id == id) {
            const auto originalStates = m_pendingStates;
            const ScheduledTask original = m_tasks.takeAt(i);
            ScheduledTask removed = original;
            removed.updatedAt = QDateTime::currentDateTimeUtc();
            removed.nextTriggerAt = {};
            queueState(removed, QStringLiteral("cancelled"));
            if (!save()) {
                m_pendingStates = originalStates;
                m_tasks.insert(i, original);
                if (errorMessage) {
                    *errorMessage = QString("无法持久化任务取消: %1").arg(m_storagePath);
                }
                return false;
            }
            synchronizeState();
            scheduleNextTick();
            emit taskChanged();
            return true;
        }
    }

    if (errorMessage) {
        *errorMessage = QString("未找到任务: %1").arg(id);
    }
    return false;
}

bool AgentScheduler::snoozeTask(const QString& id, qint64 minutes, QString* errorMessage) {
    if (!acquireStorage()) {
        if (errorMessage) *errorMessage = QStringLiteral("提醒存储不可用");
        return false;
    }
    qint64 delayMs = 0;
    if (!minutesToMs(minutes, &delayMs)) {
        if (errorMessage) *errorMessage = QStringLiteral("推迟分钟数必须是正整数且不超过 JSON 安全时间范围");
        return false;
    }
    const QDateTime now = QDateTime::currentDateTime();
    int activeIndex = -1, completedIndex = -1;
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks.at(i).id == id) { activeIndex = i; break; }
    }
    for (int i = 0; i < m_completedTasks.size(); ++i) {
        if (m_completedTasks.at(i).id == id) { completedIndex = i; break; }
    }
    if (activeIndex < 0 && completedIndex < 0) {
        if (errorMessage) *errorMessage = QString("未找到任务: %1").arg(id);
        return false;
    }
    ScheduledTask task = activeIndex >= 0 ? m_tasks.at(activeIndex) : m_completedTasks.at(completedIndex);
    task.enabled = true;
    task.nextTriggerAt = now.addMSecs(delayMs);
    task.updatedAt = now;
    if (task.triggerType == "once_at") {
        task.onceAt = task.nextTriggerAt;
        task.lastTriggeredAt = {};
    }
    QString error;
    if (!task.isValid(&error) || !task.nextTriggerAt.isValid()) {
        if (errorMessage) *errorMessage = error.isEmpty()
            ? QStringLiteral("推迟后缺少有效的下次触发时间") : error;
        return false;
    }
    const auto originalTasks = m_tasks;
    const auto originalCompleted = m_completedTasks;
    const auto originalStates = m_pendingStates;
    if (activeIndex >= 0) m_tasks[activeIndex] = task;
    else {
        m_completedTasks.removeAt(completedIndex);
        m_tasks.append(task);
    }
    queueState(task, QStringLiteral("active"), QStringLiteral("snoozed"));
    if (!save()) {
        m_tasks = originalTasks;
        m_completedTasks = originalCompleted;
        m_pendingStates = originalStates;
        if (errorMessage) *errorMessage = QString("无法持久化任务推迟: %1").arg(m_storagePath);
        return false;
    }
    synchronizeState();
    scheduleNextTick();
    emit taskChanged();
    return true;
}

void AgentScheduler::checkDueTasks() {
    checkDueTasksAt(QDateTime::currentDateTime());
}

void AgentScheduler::checkDueTasksAt(const QDateTime& now) {
    if (!m_running || !now.isValid() || !acquireStorage()) return;
    const auto originalTasks = m_tasks;
    const auto originalCompleted = m_completedTasks;
    const auto originalStates = m_pendingStates;
    const auto originalLastProactiveAt = m_lastProactiveAt;
    struct Notification { ScheduledTask task; ExecutionResult result; };
    QList<Notification> notifications;
    QList<int> dueIndices;
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks.at(i).shouldTrigger(now)) dueIndices.append(i);
    }
    std::sort(dueIndices.begin(), dueIndices.end(), [this](int left, int right) {
        const auto& a = m_tasks.at(left);
        const auto& b = m_tasks.at(right);
        if (a.nextTriggerAt != b.nextTriggerAt) return a.nextTriggerAt < b.nextTriggerAt;
        // Cooldown can align due times. Let never-delivered / least-recently
        // delivered peers go first so recurring high-priority work cannot
        // repeatedly displace a waiting reminder.
        if (a.lastTriggeredAt.isValid() != b.lastTriggeredAt.isValid())
            return !a.lastTriggeredAt.isValid();
        if (a.lastTriggeredAt != b.lastTriggeredAt) return a.lastTriggeredAt < b.lastTriggeredAt;
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.id < b.id;
    });
    QStringList completedIds;
    bool changed = false;
    for (const int index : dueIndices) {
        auto& task = m_tasks[index];
        if (task.respectQuietHours && isInQuietHours(now)) {
            const QDate resumeDate = now.time() >= QTime(23, 30)
                ? now.date().addDays(1) : now.date();
            task.nextTriggerAt = QDateTime(resumeDate, QTime(8, 0), now.timeZone());
        } else if (task.skipWhenUserBusy && (!m_userBusyProvider || m_userBusyProvider())) {
            task.nextTriggerAt = now.addSecs(60);
        } else if (task.lastTriggeredAt.isValid()
                   && task.lastTriggeredAt.msecsTo(now) < task.minGapMs) {
            task.nextTriggerAt = task.lastTriggeredAt.addMSecs(task.minGapMs);
        } else if (task.source != QLatin1String("user_request") && m_lastProactiveAt.isValid()
                   && m_lastProactiveAt.msecsTo(now) < m_minProactiveGapMs) {
            task.nextTriggerAt = m_lastProactiveAt.addMSecs(m_minProactiveGapMs);
        } else {
            auto result = executeTask(task, now);
            notifications.append({task, result});
            task.updatedAt = now;
            if (result.delivered) {
                task.lastTriggeredAt = now;
                if (task.source != QLatin1String("user_request")) m_lastProactiveAt = now;
                if (task.triggerType == "once_at") {
                    task.enabled = false;
                    task.nextTriggerAt = {};
                    completedIds.append(task.id);
                } else task.refreshNextTrigger(now);
                queueState(task, task.triggerType == "once_at" ? "completed" : "active",
                           result.error.isEmpty() ? "delivered" : "partially_delivered");
            } else {
                task.nextTriggerAt = now.addSecs(60); // Retry failed notification; do not mark complete.
                queueState(task, "active", result.deferred ? "deferred" : "retry_pending");
            }
            changed = true;
            continue;
        }
        task.updatedAt = now;
        queueState(task, "active", "deferred");
        changed = true;
    }
    for (const auto& id : completedIds) {
        const auto completed = std::find_if(m_tasks.begin(), m_tasks.end(),
                                             [&id](const auto& task) { return task.id == id; });
        if (completed != m_tasks.end()) {
            m_completedTasks.append(*completed);
            m_tasks.erase(completed);
        }
    }
    while (m_completedTasks.size() > kCompletedTaskLimit) m_completedTasks.removeFirst();
    if (changed) {
        if (!save()) {
            m_tasks = originalTasks;
            m_completedTasks = originalCompleted;
            m_pendingStates = originalStates;
            m_lastProactiveAt = originalLastProactiveAt;
            stop();
            emit taskFailed({}, QStringLiteral("无法持久化调度状态，调度器已停止"));
            return;
        }
        for (const auto& notice : notifications) {
            if (notice.result.delivered) {
                if (!notice.result.error.isEmpty())
                    emit taskPartiallySucceeded(notice.task.id, notice.result.error);
                emit taskTriggered(notice.task.id, notice.task.title);
            } else if (!notice.result.deferred) emit taskFailed(notice.task.id, notice.result.error);
        }
        emit taskChanged();
    }
    synchronizeState();
    scheduleNextTick();
}

AgentScheduler::ExecutionResult AgentScheduler::executeTask(ScheduledTask& task, const QDateTime&) {
    if (!m_toolRegistry) return {false, QStringLiteral("ToolRegistry 未配置")};
    auto executeTool = [this](const QString& name, const QJsonObject& arguments) {
        ToolExecutionRequest request;
        request.toolName = name;
        request.arguments = arguments;
        request.policyContext.triggerTag = "schedule";
        request.policyContext.initiatedByLlm = false;
        return m_toolRuntime.execute(request).result;
    };
    bool delivered = false;
    if (!task.message.trimmed().isEmpty()) {
        const auto result = executeTool("show_chat_bubble", {{"text", task.message.trimmed()}});
        if (!result.success) {
            // A busy UI has not accepted the reminder. Leave optional actions
            // untouched and retry without reporting a delivery failure.
            return {false, result.errorMessage, result.data.value("delivery_deferred").toBool()};
        }
        delivered = true;
    }
    if (!task.animationState.trimmed().isEmpty()) {
        const auto result = executeTool("play_animation", {{"state", task.animationState.trimmed()}});
        if (!result.success) return {delivered, result.errorMessage};
        delivered = true;
    }
    return {delivered, {}};
}

bool AgentScheduler::isInQuietHours(const QDateTime& now) const {
    const QTime start(23, 30);
    const QTime end(8, 0);
    const QTime current = now.time();
    return current >= start || current < end;
}

void AgentScheduler::scheduleNextTick() {
    if (!m_running) {
        return;
    }

    int nextMs = 60 * 1000;
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime due = nearestDueAt();
    if (due.isValid()) {
        const qint64 diff = now.msecsTo(due);
        if (diff <= 0) {
            nextMs = 1000;
        } else {
            nextMs = qMin(nextMs, static_cast<int>(qMin<qint64>(diff, 60 * 1000)));
        }
    }
    m_timer.start(qMax(1000, nextMs));
}

QDateTime AgentScheduler::nearestDueAt() const {
    QDateTime nearest;
    for (const ScheduledTask& task : m_tasks) {
        if (!task.enabled || !task.nextTriggerAt.isValid()) {
            continue;
        }
        if (!nearest.isValid() || task.nextTriggerAt < nearest) {
            nearest = task.nextTriggerAt;
        }
    }
    return nearest;
}

qint64 AgentScheduler::msToNextDue() const {
    const QDateTime due = nearestDueAt();
    if (!due.isValid()) return -1; // 无待办
    const qint64 diff = QDateTime::currentDateTime().msecsTo(due);
    return diff < 0 ? 0 : diff; // 已过期算 0
}

bool AgentScheduler::hasTaskDueBefore(const QDateTime& boundary) const {
    if (!boundary.isValid()) return false;
    const QDateTime due = nearestDueAt();
    return due.isValid() && due <= boundary;
}

QString AgentScheduler::defaultStoragePath() {
    const QString configDir = QDir::current().filePath("config");
    QDir().mkpath(configDir);
    return QDir(configDir).filePath("scheduled_tasks.json");
}
