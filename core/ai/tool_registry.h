//
// Created by Inoriac on 2026/4/8.
// Tool 注册中心
// 管理所有 Tool 实例的注册、查找、Schema 导出和执行分发
//

#ifndef DESKTOP_PET_TOOL_REGISTRY_H
#define DESKTOP_PET_TOOL_REGISTRY_H

#include <map>
#include <QJsonArray>
#include <QStringList>
#include <memory>

#include "ai_tool.h"

class ToolRegistry {
public:
    ToolRegistry() = default;
    ~ToolRegistry() = default;

    // 注册一个 Tool（转移所有权给 Registry）
    // 使用 unique_ptr 确保生命周期由 Registry 管理
    void registerTool(std::unique_ptr<AITool> tool);

    // 根据名字查找 Tool（返回裸指针，所有权仍在 Registry）
    AITool* getTool(const QString& name) const;

    // 检查是否存在某个 Tool
    bool hasTool(const QString& name) const;

    // 导出所有 Tool 的 Schema，用于发给 LLM
    // 返回的 QJsonArray 就是 OpenAI API 的 "tools" 参数
    QJsonArray allToolSchemas() const;

    // 执行一个 Tool
    // name: LLM 返回的 function name
    // arguments: LLM 返回的 function arguments（JSON 对象）
    // 内部做 查找 → 校验 → 执行 的完整流程
    ToolResult executeTool(const QString& name, const QJsonObject& arguments);

    // 获取已注册的 Tool 数量
    int toolCount() const;

    // 获取所有已注册的 Tool 名称（调试用）
    QStringList toolNames() const;

private:
    // name -> tool 的映射
    // 用 unique_ptr 管理生命周期，Registry 销毁时自动释放所有 Tool
    // 使用 std::map 而非 QMap，因为 QMap 不支持 move-only 类型 (unique_ptr)
    std::map<QString, std::unique_ptr<AITool>> m_tools;
};

#endif // DESKTOP_PET_TOOL_REGISTRY_H