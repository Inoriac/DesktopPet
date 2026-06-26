//
// Skill management tools — exposed to LLM for autonomous skill CRUD
//

#ifndef DESKTOP_PET_SKILL_TOOLS_H
#define DESKTOP_PET_SKILL_TOOLS_H

#include "../ai_tool.h"

class SkillStore;

class SkillCreateTool : public AITool {
public:
    explicit SkillCreateTool(SkillStore* store);
    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    SkillStore* m_store;
};

class SkillUpdateTool : public AITool {
public:
    explicit SkillUpdateTool(SkillStore* store);
    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    SkillStore* m_store;
};

class SkillListTool : public AITool {
public:
    explicit SkillListTool(SkillStore* store);
    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    SkillStore* m_store;
};

class SkillDeleteTool : public AITool {
public:
    explicit SkillDeleteTool(SkillStore* store);
    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    SkillStore* m_store;
};

class SkillRecordOutcomeTool : public AITool {
public:
    explicit SkillRecordOutcomeTool(SkillStore* store);
    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    SkillStore* m_store;
};

#endif // DESKTOP_PET_SKILL_TOOLS_H
