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

void AgentScheduler::setToolRegistry(ToolRegistry* registry) {
    m_toolRegistry = registry;
    m_toolRuntime.setToolRegistry(registry);
}

void AgentScheduler::setStoragePath(const QString& storagePath) {
    if (!storagePath.trimmed().isEmpty()) {
        m_storagePath = storagePath;
    }
}

bool AgentScheduler::load() {
    m_tasks.clear();

    QFile file(m_storagePath);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        return false;
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
    if (!QDir().mkpath(QFileInfo(m_storagePath).absolutePath())) {
        return false;
    }

    QJsonArray items;
    for (const ScheduledTask& task : m_tasks) {
        items.append(task.toJson());
    }

    QJsonObject root;
    root["version"] = 1;
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
    m_running = true;
    scheduleNextTick();
}

void AgentScheduler::stop() {
    m_running = false;
    m_timer.stop();
}

ScheduledTask AgentScheduler::createTask(const QJsonObject& params, QString* errorMessage) {
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

    task.respectQuietHours = params.value("respect_quiet_hours").toBool(true);
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

    m_tasks.append(task);
    if (!save()) {
        m_tasks.removeLast();
        if (errorMessage) {
            *errorMessage = QString("无法持久化调度任务: %1").arg(m_storagePath);
        }
        return {};
    }
    scheduleNextTick();
    emit taskChanged();
    return task;
}

bool AgentScheduler::cancelTask(const QString& id, QString* errorMessage) {
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].id == id) {
            const ScheduledTask removed = m_tasks.takeAt(i);
            if (!save()) {
                m_tasks.insert(i, removed);
                if (errorMessage) {
                    *errorMessage = QString("无法持久化任务取消: %1").arg(m_storagePath);
                }
                return false;
            }
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
    const QDateTime now = QDateTime::currentDateTime();
    for (ScheduledTask& task : m_tasks) {
        if (task.id == id) {
            const ScheduledTask original = task;
            task.enabled = true;
            task.nextTriggerAt = now.addMSecs(minutesToMs(minutes));
            task.updatedAt = now;
            if (!save()) {
                task = original;
                if (errorMessage) {
                    *errorMessage = QString("无法持久化任务推迟: %1").arg(m_storagePath);
                }
                return false;
            }
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
    const QDateTime now = QDateTime::currentDateTime();
    const QList<ScheduledTask> originalTasks = m_tasks;
    const QDateTime originalLastProactiveAt = m_lastProactiveAt;
    bool changed = false;

    for (ScheduledTask& task : m_tasks) {
        if (!task.shouldTrigger(now)) {
            continue;
        }

        if (task.respectQuietHours && isInQuietHours(now)) {
            task.nextTriggerAt = now.addSecs(30 * 60);
            changed = true;
            continue;
        }

        if (m_lastProactiveAt.isValid() && m_lastProactiveAt.msecsTo(now) < m_minProactiveGapMs) {
            task.nextTriggerAt = now.addMSecs(m_minProactiveGapMs - m_lastProactiveAt.msecsTo(now));
            changed = true;
            continue;
        }

        executeTask(task, now);
        changed = true;
    }

    for (int i = m_tasks.size() - 1; i >= 0; --i) {
        const ScheduledTask& task = m_tasks[i];
        if (task.triggerType == "once_at" && task.lastTriggeredAt.isValid()) {
            m_tasks.removeAt(i);
            changed = true;
        }
    }

    if (changed) {
        if (!save()) {
            m_tasks = originalTasks;
            m_lastProactiveAt = originalLastProactiveAt;
            m_running = false;
            m_timer.stop();
            emit taskFailed({}, QString("无法持久化调度状态，调度器已停止: %1").arg(m_storagePath));
            emit taskChanged();
            return;
        }
        emit taskChanged();
    }
    scheduleNextTick();
}

void AgentScheduler::executeTask(ScheduledTask& task, const QDateTime& now) {
    if (!m_toolRegistry) {
        emit taskFailed(task.id, "ToolRegistry 未配置");
        return;
    }

    auto executeTool = [this](const QString& toolName, const QJsonObject& arguments) -> ToolResult {
        ToolExecutionRequest request;
        request.toolName = toolName;
        request.arguments = arguments;
        request.policyContext.triggerTag = "schedule";
        request.policyContext.initiatedByLlm = false;
        return m_toolRuntime.execute(request).result;
    };

    if (!task.animationState.trimmed().isEmpty()) {
        QJsonObject animationArgs;
        animationArgs["state"] = task.animationState.trimmed();
        const ToolResult animationResult = executeTool("play_animation", animationArgs);
        if (!animationResult.success) {
            emit taskFailed(task.id, animationResult.errorMessage);
        }
    }

    if (!task.message.trimmed().isEmpty()) {
        QJsonObject bubbleArgs;
        bubbleArgs["text"] = task.message.trimmed();
        const ToolResult bubbleResult = executeTool("show_chat_bubble", bubbleArgs);
        if (!bubbleResult.success) {
            emit taskFailed(task.id, bubbleResult.errorMessage);
        }
    }

    task.lastTriggeredAt = now;
    task.updatedAt = now;
    m_lastProactiveAt = now;

    if (task.triggerType == "daily_at" || task.triggerType == "interval") {
        task.refreshNextTrigger(now);
    }

    emit taskTriggered(task.id, task.title);
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
    for (const ScheduledTask& task : m_tasks) {
        if (!task.enabled || !task.nextTriggerAt.isValid()) {
            continue;
        }
        const qint64 diff = now.msecsTo(task.nextTriggerAt);
        if (diff <= 0) {
            nextMs = 1000;
            break;
        }
        nextMs = qMin(nextMs, static_cast<int>(qMin<qint64>(diff, 60 * 1000)));
    }
    m_timer.start(qMax(1000, nextMs));
}

QString AgentScheduler::defaultStoragePath() {
    const QString configDir = QDir::current().filePath("config");
    QDir().mkpath(configDir);
    return QDir(configDir).filePath("scheduled_tasks.json");
}
