#pragma once

#include <QObject>
#include <QThread>
#include <QHash>
#include <atomic>
#include <memory>
#include <functional>
#include "tool_runtime.h"

// Stateless blocking tools own their network/event-loop objects on this worker.
// Scheduler/UI tools retain their existing owner thread and are dispatched separately.
class TaskToolExecutor : public QObject {
public:
    explicit TaskToolExecutor(QObject* parent = nullptr);
    ~TaskToolExecutor() override;
    static bool supports(const QString& name);
    void execute(ToolExecutionRequest request,
                 std::function<void(ToolExecutionOutcome)> completion);
    void cancel(const QString& id);
private:
    QThread m_thread;
    QObject* m_worker = nullptr;
    QHash<QString, std::shared_ptr<std::atomic_bool>> m_cancelled;
};
