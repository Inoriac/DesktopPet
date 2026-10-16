#ifndef DESKTOP_PET_TOOL_REGISTRY_H
#define DESKTOP_PET_TOOL_REGISTRY_H
#include <map>
#include <QJsonArray>
#include <QStringList>
#include <memory>
#include <functional>
#include <atomic>
#include <optional>
#include "ai_tool.h"
struct ToolExecutionRequest;
struct ToolExecutionOutcome;
class ToolRegistry {
public:
    ~ToolRegistry();
    using WorkerFactory=std::function<std::unique_ptr<AITool>()>;
    using AsyncExecutor=std::function<void(const ToolExecutionRequest&,std::function<void(ToolExecutionOutcome)>)>;
    struct ExecutionBinding {
        WorkerFactory factory;
        AsyncExecutor async;
        std::shared_ptr<std::atomic_bool> enabled;
        HostToolMetadata metadata;
    };
    bool registerTool(std::unique_ptr<AITool> tool,std::optional<HostToolMetadata> metadata={},WorkerFactory factory={},AsyncExecutor async={});
    bool unregisterTool(const QString& name);
    bool setToolEnabled(const QString& name,bool enabled);
    AITool* getTool(const QString& name) const;
    bool hasTool(const QString& name) const;
    QJsonArray allToolSchemas() const;
    QJsonArray toolSchemas(const QStringList& names) const;
    ToolResult executeTool(const QString& name,const QJsonObject& arguments);
    int toolCount() const;
    QStringList toolNames() const;
    ExecutionBinding executionBinding(const QString& name) const;
    bool supportsAsync(const QString& name) const;
private:
    struct Registration { std::unique_ptr<AITool> tool; ExecutionBinding binding; };
    std::map<QString,Registration> m_tools;
};
#endif
