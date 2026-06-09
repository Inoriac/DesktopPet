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

signals:
    void taskTriggered(const QString& id, const QString& title);
    void taskFailed(const QString& id, const QString& errorMessage);
    void taskChanged();

private:
    void checkDueTasks();
    void executeTask(ScheduledTask& task, const QDateTime& now);
    bool isInQuietHours(const QDateTime& now) const;
    void scheduleNextTick();
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
