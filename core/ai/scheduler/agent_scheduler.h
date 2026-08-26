//
// Agent 主动调度器
//

#ifndef DESKTOP_PET_AGENT_SCHEDULER_H
#define DESKTOP_PET_AGENT_SCHEDULER_H

#include "scheduled_task.h"
#include "ai/tool_registry.h"
#include "ai/tools/runtime/tool_runtime.h"

#include <QObject>
#include <QTimer>

class AgentScheduler : public QObject {
    Q_OBJECT

public:
    explicit AgentScheduler(QObject* parent = nullptr);

    void setToolRegistry(ToolRegistry* registry);
    void setStoragePath(const QString& storagePath);
    QString storagePath() const { return m_storagePath; }

    bool load();
    bool save() const;

    void start();
    void stop();
    bool isRunning() const { return m_running; }

    ScheduledTask createTask(const QJsonObject& params, QString* errorMessage = nullptr);
    QList<ScheduledTask> tasks() const { return m_tasks; }
    bool cancelTask(const QString& id, QString* errorMessage = nullptr);
    bool snoozeTask(const QString& id, int minutes, QString* errorMessage = nullptr);

    // 距最近一个待办 due 的毫秒数（已过期返回 0，无待办返回 -1）。不封顶，
    // 供 Daydream 触发判定「距待办 > N₂ 分钟」用。与 scheduleNextTick 同源。
    qint64 msToNextDue() const;
    bool hasTaskDueBefore(const QDateTime& boundary) const;

signals:
    void taskTriggered(const QString& id, const QString& title);
    void taskFailed(const QString& id, const QString& errorMessage);
    void taskChanged();

private:
    void checkDueTasks();
    void executeTask(ScheduledTask& task, const QDateTime& now);
    bool isInQuietHours(const QDateTime& now) const;
    void scheduleNextTick();
    // 最近 enabled 且 nextTriggerAt 有效任务的 due 时间；无则 invalid。
    // scheduleNextTick 与 msToNextDue 共享此过滤，避免双份维护。
    QDateTime nearestDueAt() const;
    static QString defaultStoragePath();

private:
    QList<ScheduledTask> m_tasks;
    ToolRegistry* m_toolRegistry = nullptr; // non-owning
    ToolRuntime m_toolRuntime;
    QTimer m_timer;
    QString m_storagePath;
    bool m_running = false;
    QDateTime m_lastProactiveAt;
    int m_minProactiveGapMs = 10 * 60 * 1000;
};

#endif // DESKTOP_PET_AGENT_SCHEDULER_H
