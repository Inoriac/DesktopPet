//
// Created by Huang_cj on 2026/4/8.
// Tool 注册中心 - 实现
//

#include "tool_registry.h"
#include <QDebug>

void ToolRegistry::registerTool(std::unique_ptr<AITool> tool) {
    if (!tool) {
        qWarning() << "[ToolRegistry] Attempted to register null tool";
        return;
    }

    const QString toolName = tool->name();

    if (m_tools.contains(toolName)) {
        qWarning() << "[ToolRegistry] Tool already registered:" << toolName << ", replacing";
    }

    qDebug() << "[ToolRegistry] Registered tool:" << toolName;
    m_tools[toolName] = std::move(tool);
}

AITool* ToolRegistry::getTool(const QString& name) const {
    auto it = m_tools.find(name);
    return (it != m_tools.end()) ? it->second.get() : nullptr;
}

bool ToolRegistry::hasTool(const QString& name) const {
    return m_tools.find(name) != m_tools.end();
}

QJsonArray ToolRegistry::allToolSchemas() const {
    QJsonArray schemas;
    for (const auto& [name, tool] : m_tools) {
        schemas.append(tool->toFunctionSchema());
    }
    return schemas;
}

ToolResult ToolRegistry::executeTool(const QString& name, const QJsonObject& arguments) {
    // 1. 查找 Tool
    AITool* tool = getTool(name);
    if (!tool) {
        return ToolResult::fail(QString("Unknown tool: %1").arg(name));
    }

    // 2. 参数校验
    if (!tool->validate(arguments)) {
        return ToolResult::fail(
            QString("Invalid parameters for tool '%1': missing required fields").arg(name)
        );
    }

    // 3. 执行
    qDebug() << "[ToolRegistry] Executing tool:" << name;
    return tool->execute(arguments);
}

int ToolRegistry::toolCount() const {
    return static_cast<int>(m_tools.size());
}

QStringList ToolRegistry::toolNames() const {
    QStringList names;
    for (const auto& [name, tool] : m_tools) {
        names.append(name);
    }
    return names;
}