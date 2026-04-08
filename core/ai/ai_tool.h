//
// Created by Inoriac on 2026/4/8.
// AI Tool 抽象基类
// 所有 Tool 继承此类，实现 parameterSchema() 和 execute()
// 基类负责把 name + description + schema 组装成 OpenAI function calling 格式
//
#ifndef DESKTOP_PET_AI_TOOL_H
#define DESKTOP_PET_AI_TOOL_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>

#include "ai_types.h"

class AITool {
public:
    // ================================================================
    // 构造函数：每个 Tool 在创建时就确定了名称、描述和类别
    // name: 工具唯一标识符，如 "play_animation"（LLM 通过这个名字来调用）
    // description: 给 LLM 看的自然语言描述，越精确 LLM 选择越准
    // category: 查询类 or 操作类
    // ================================================================
    AITool(QString name, QString description, ToolCategory category = ToolCategory::Action)
        : m_name(std::move(name))
        , m_description(std::move(description))
        , m_category(category) {}

    virtual ~AITool() = default;

    // 返回参数的 JSON Schema
    // 格式遵循 OpenAI function calling 的 parameters 字段规范
    // 例如:
    // {
    //   "type": "object",
    //   "properties": {
    //     "state": { "type": "string", "description": "目标动画状态" }
    //   },
    //   "required": ["state"]
    // }
    virtual QJsonObject parameterSchema() const = 0;

    // 执行工具
    // params: LLM 传来的参数 JSON（已经过 validate 校验）
    // 返回: ToolResult，包含成功/失败状态和数据
    virtual ToolResult execute(const QJsonObject& params) = 0;

    // 参数校验：检查 required 字段是否都存在
    // 子类可以重写以添加更复杂的校验逻辑（如 enum 范围检查）
    virtual bool validate(const QJsonObject& params) const {
        QJsonObject schema = parameterSchema();
        QJsonArray required = schema.value("required").toArray();

        for (const auto& field : required) {
            if (!params.contains(field.toString())) {
                return false;
            }
        }
        return true;
    }

    // 组装成完整的 OpenAI function calling 格式
    // 这个方法由 ToolRegistry 调用，用于构建发给 LLM 的 tools 数组
    // 输出格式:
    // {
    //   "type": "function",
    //   "function": {
    //     "name": "play_animation",
    //     "description": "切换桌宠的动画状态...",
    //     "parameters": { ... schema ... }
    //   }
    // }
    QJsonObject toFunctionSchema() const {
        QJsonObject function;
        function["name"] = m_name;
        function["description"] = m_description;
        function["parameters"] = parameterSchema();

        QJsonObject tool;
        tool["type"] = "function";
        tool["function"] = function;
        return tool;
    }

    const QString& name() const { return m_name; }
    const QString& description() const { return m_description; }
    ToolCategory category() const { return m_category; }

protected:
    QString m_name;
    QString m_description;
    ToolCategory m_category;
};

#endif //DESKTOP_PET_AI_TOOL_H