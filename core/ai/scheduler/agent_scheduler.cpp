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
#include <QTime>
#include <QTimeZone>
#include <QUuid>

namespace {
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

int minutesToMs(int minutes) {
    return qMax(1, minutes) * 60 * 1000;
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
        if (!m_pendingStates.contains(task.id)) queueState(task, QStringLiteral("active"));
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
    m_tasks.clear();
    m_pendingStates.clear();

    QFile file(m_storagePath);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_storageReady = false;
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        m_storageReady = false;
        return false;
    }

    for (const auto& value : doc.object().value("pending_states").toArray()) {
        const auto state = value.toObject();
        const QString id = state.value("id").toString();
        if (!id.isEmpty()) m_pendingStates.insert(id, state);
    }
    const QJsonArray items = doc.object().value("tasks").toArray();
    const QDateTime now = QDateTime::currentDateTime();
    for (const QJsonValue& value : items) {
        ScheduledTask task = ScheduledTask::fromJson(value.toObject());
        if (!task.nextTriggerAt.isValid()) {
            task.refreshNextTrigger(now);
        }
        QString error;
        if (task.isValid(&error)) {
            m_tasks.append(task);
        }
    }
    return true;
}

bool AgentScheduler::save() const {
    if (!acquireStorage()) return false;
    if (!QDir().mkpath(QFileInfo(m_storagePath).absolutePath())) {
        return false;
    }

    QJsonArray items;
    for (const ScheduledTask& task : m_tasks) {
        items.append(task.toJson());
    }

    QJsonObject root;
    root["version"] = 2;
    QJsonArray pending;
    for (const auto& state : m_pendingStates) pending.append(state);
    root["pending_states"] = pending;
    root["tasks"] = items;

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
        const int delayMinutes = params.value("delay_minutes").toInt(0);
        if (delayMinutes > 0) {
            task.onceAt = now.addMSecs(minutesToMs(delayMinutes));
        } else {
            task.onceAt = parseDateTime(params.value("at").toString(params.value("time").toString()), now);
        }
    } else if (task.triggerType == "daily_at") {
        task.dailyAt = parseTime(params.value("time").toString(params.value("at").toString()));
    } else if (task.triggerType == "interval") {
        int intervalMs = params.value("interval_ms").toInt(0);
        if (intervalMs <= 0) {
            intervalMs = minutesToMs(params.value("interval_minutes").toInt(0));
        }
        task.intervalMs = intervalMs;
    }

    const auto quiet = params.value("respect_quiet_hours");
    task.respectQuietHours = quiet.isDouble() ? quiet.toInt() != 0 : quiet.toBool(true);
    task.skipWhenUserBusy = params.value("skip_when_user_busy").toBool(false);
    task.minGapMs = params.value("min_gap_ms").toInt(0);
    task.allowLlm = false;
    task.allowNetwork = false;
    task.createdAt = now;
    task.updatedAt = now;
    task.refreshNextTrigger(now);

    QString error;
    if (!task.isValid(&error)) {
        if (errorMessage) {
            *errorMessage = error;
        }
        return {};
    }

    for (const auto& existing : m_tasks) {
        if (existing.id == task.id) {
            if (errorMessage) *errorMessage = QStringLiteral("提醒 id 已存在");
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

bool AgentScheduler::snoozeTask(const QString& id, int minutes, QString* errorMessage) {
    if (!acquireStorage()) {
        if (errorMessage) *errorMessage = QStringLiteral("提醒存储不可用");
        return false;
    }
    const QDateTime now = QDateTime::currentDateTime();
    for (ScheduledTask& task : m_tasks) {
        if (task.id == id) {
            const ScheduledTask original = task;
            const auto originalStates = m_pendingStates;
            task.enabled = true;
            task.nextTriggerAt = now.addMSecs(minutesToMs(minutes));
            task.updatedAt = now;
            queueState(task, QStringLiteral("active"), QStringLiteral("snoozed"));
            if (!save()) {
                m_pendingStates = originalStates;
                task = original;
                if (errorMessage) {
                    *errorMessage = QString("无法持久化任务推迟: %1").arg(m_storagePath);
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

void AgentScheduler::checkDueTasks() {
    if (!m_running || !acquireStorage()) return;
    const QDateTime now = QDateTime::currentDateTime();
    const auto originalTasks = m_tasks;
    const auto originalStates = m_pendingStates;
    const auto originalLastProactiveAt = m_lastProactiveAt;
    struct Notification { ScheduledTask task; ExecutionResult result; };
    QList<Notification> notifications;
    bool changed = false;
    for (auto& task : m_tasks) {
        if (!task.shouldTrigger(now)) continue;
        if (task.respectQuietHours && isInQuietHours(now)) {
            task.nextTriggerAt = now.addSecs(30 * 60);
        } else if (m_lastProactiveAt.isValid()
                   && m_lastProactiveAt.msecsTo(now) < m_minProactiveGapMs) {
            task.nextTriggerAt = m_lastProactiveAt.addMSecs(m_minProactiveGapMs);
        } else {
            auto result = executeTask(task, now);
            notifications.append({task, result});
            task.updatedAt = now;
            if (result.delivered) {
                task.lastTriggeredAt = now;
                m_lastProactiveAt = now;
                if (task.triggerType == "once_at") task.nextTriggerAt = {};
                else task.refreshNextTrigger(now);
                queueState(task, task.triggerType == "once_at" ? "completed" : "active",
                           result.error.isEmpty() ? "delivered" : "partially_delivered");
            } else {
                task.nextTriggerAt = now.addSecs(60); // Retry failed notification; do not mark complete.
                queueState(task, "active", "retry_pending");
            }
            changed = true;
            continue;
        }
        task.updatedAt = now;
        queueState(task, "active", "deferred");
        changed = true;
    }
    for (int i = m_tasks.size() - 1; i >= 0; --i) {
        if (m_tasks[i].triggerType == "once_at" && m_tasks[i].lastTriggeredAt.isValid())
            m_tasks.removeAt(i);
    }
    if (changed) {
        if (!save()) {
            m_tasks = originalTasks;
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
            } else emit taskFailed(notice.task.id, notice.result.error);
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
    QStringList errors;
    bool animationDelivered = false;
    if (!task.animationState.trimmed().isEmpty()) {
        const auto result = executeTool("play_animation", {{"state", task.animationState.trimmed()}});
        animationDelivered = result.success;
        if (!result.success) errors.append(result.errorMessage);
    }
    bool delivered = animationDelivered;
    if (!task.message.trimmed().isEmpty()) {
        const auto result = executeTool("show_chat_bubble", {{"text", task.message.trimmed()}});
        delivered = result.success; // The optional animation cannot substitute for the reminder text.
        if (!result.success) errors.append(result.errorMessage);
    }
    return {delivered, errors.join(QStringLiteral("; "))};
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
