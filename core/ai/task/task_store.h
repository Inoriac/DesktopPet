#pragma once
#include "ai/activity/activity_store.h"
#include "task_types.h"
#include <memory>
#include <optional>

// Each checkpoint and its terminal experience intent share one runtime DB transaction.
class TaskStore {
public:
    TaskStore() = default;
    ~TaskStore();
    TaskStore(const TaskStore&) = delete;
    TaskStore& operator=(const TaskStore&) = delete;
    bool open(const QString& databasePath);
    bool open(const QString &databasePath, const QString &profileId);
    ActivityStore *activityStore() const { return m_activity.get(); }
    QList<TaskRecord> unfinishedTasks() const;
    void close();
    bool isOpen() const;
    QString lastError() const { return m_error; }
    bool save(TaskRecord &task, bool enqueueExperience = false);
    bool save(const TaskRecord& task, bool enqueueExperience = false);
    std::optional<TaskRecord> unfinished() const;
    std::optional<TaskRecord> find(const QString& id) const;
    QJsonArray pendingExperiences() const;
    bool acknowledgeExperience(const QString& taskId);
private:
  std::unique_ptr<ActivityStore> m_activity;
  QString m_connectionName;
  mutable QString m_error;
};
