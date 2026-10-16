#include "task_tool_executor.h"
bool TaskToolExecutor::supports(const QString& name) const { return m_runtime && m_runtime->toolRegistry() && m_runtime->toolRegistry()->supportsAsync(name); }
void TaskToolExecutor::execute(ToolExecutionRequest request,std::function<void(ToolExecutionOutcome)> completion) {
    if (m_runtime) m_runtime->executeAsync(request,std::move(completion));
    else { ToolExecutionOutcome out; out.requestId=request.requestId; out.toolName=request.toolName; out.result=ToolResult::fail("Tool runtime unavailable"); completion(out); }
}
void TaskToolExecutor::cancel(const QString& id) { if (m_runtime) m_runtime->cancel(id); }
