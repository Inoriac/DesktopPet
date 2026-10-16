#include "tool_registry.h"
#include <QDebug>
ToolRegistry::~ToolRegistry() {
    for (const auto& [name,entry]:m_tools) entry.binding.enabled->store(false);
}
bool ToolRegistry::registerTool(std::unique_ptr<AITool> tool,std::optional<HostToolMetadata> metadata,WorkerFactory factory,AsyncExecutor async) {
    if (!tool || tool->name().isEmpty() || m_tools.contains(tool->name())) return false;
    if (metadata) tool->setHostMetadata(*metadata);
    const auto name=tool->name();
    ExecutionBinding binding{std::move(factory),std::move(async),std::make_shared<std::atomic_bool>(true),tool->hostMetadata()};
    m_tools.emplace(name,Registration{std::move(tool),std::move(binding)}); return true;
}
bool ToolRegistry::unregisterTool(const QString& name) {
    auto it=m_tools.find(name); if (it==m_tools.end()) return false;
    it->second.binding.enabled->store(false); m_tools.erase(it); return true;
}
bool ToolRegistry::setToolEnabled(const QString& name,bool enabled) {
    auto it=m_tools.find(name); if (it==m_tools.end()) return false;
    // Replace the token so re-enabling cannot revive old queued calls.
    it->second.binding.enabled->store(false); it->second.binding.enabled=std::make_shared<std::atomic_bool>(enabled); return true;
}
AITool* ToolRegistry::getTool(const QString& name) const {
    auto it=m_tools.find(name); return it==m_tools.end() || !it->second.binding.enabled->load()?nullptr:it->second.tool.get();
}
bool ToolRegistry::hasTool(const QString& name) const { return getTool(name)!=nullptr; }
QJsonArray ToolRegistry::allToolSchemas() const { return toolSchemas(toolNames()); }
QJsonArray ToolRegistry::toolSchemas(const QStringList& names) const {
    QJsonArray result; for (const auto& name:names) if (const auto* tool=getTool(name)) result.append(tool->toFunctionSchema()); return result;
}
ToolResult ToolRegistry::executeTool(const QString& name,const QJsonObject& args) {
    auto* tool=getTool(name); if (!tool) return ToolResult::fail(QString("Unknown tool: %1").arg(name));
    if (!tool->validate(args)) return ToolResult::fail(QString("Invalid parameters for tool '%1': missing required fields").arg(name));
    return tool->execute(args);
}
int ToolRegistry::toolCount() const { return int(m_tools.size()); }
QStringList ToolRegistry::toolNames() const {
    QStringList result; for (const auto& [name,entry]:m_tools) if (entry.binding.enabled->load()) result.append(name); return result;
}
ToolRegistry::ExecutionBinding ToolRegistry::executionBinding(const QString& name) const {
    auto it=m_tools.find(name); return it==m_tools.end()?ExecutionBinding{}:it->second.binding;
}
bool ToolRegistry::supportsAsync(const QString& name) const {
    if (!hasTool(name)) return false;
    const auto binding=executionBinding(name);
    return binding.metadata.executor==HostToolExecutor::OwnerThread || (binding.metadata.executor==HostToolExecutor::Worker && bool(binding.factory)) || (binding.metadata.executor==HostToolExecutor::Async && bool(binding.async));
}
