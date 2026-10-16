#pragma once
#include <QObject>
#include "tool_runtime.h"
// Compatibility facade: registration owns factory, thread choice and policy.
class TaskToolExecutor : public QObject {
public:
    explicit TaskToolExecutor(ToolRuntime* runtime,QObject* parent=nullptr):QObject(parent),m_runtime(runtime) {}
    bool supports(const QString& name) const;
    void execute(ToolExecutionRequest request,std::function<void(ToolExecutionOutcome)> completion);
    void cancel(const QString& id);
private:
    ToolRuntime* m_runtime=nullptr;
};
