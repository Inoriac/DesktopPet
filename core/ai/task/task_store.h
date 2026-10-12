#pragma once
#include "task_types.h"
#include <optional>

// Each checkpoint and its terminal experience intent share one runtime DB transaction.
class TaskStore {
public:
    TaskStore() = default;
    ~TaskStore();
    TaskStore(const TaskStore&) = delete;
    TaskStore& operator=(const TaskStore&) = delete;
    bool open(const QString& databasePath);
    void close();
    bool isOpen() const;
    QString lastError() const { return m_error; }
    bool save(const TaskRecord& task, bool enqueueExperience = false);
    std::optional<TaskRecord> unfinished() const;
    std::optional<TaskRecord> find(const QString& id) const;
    QJsonArray pendingExperiences() const;
    bool acknowledgeExperience(const QString& taskId);
private:
    QString m_connectionName;
    mutable QString m_error;
};
