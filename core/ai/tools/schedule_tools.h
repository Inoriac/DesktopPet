//
// 定时与提醒工具
//

#ifndef DESKTOP_PET_SCHEDULE_TOOLS_H
#define DESKTOP_PET_SCHEDULE_TOOLS_H

#include "../ai_tool.h"
#include "ai/scheduler/agent_scheduler.h"

class ScheduleCreateTool : public AITool {
public:
    explicit ScheduleCreateTool(AgentScheduler* scheduler);

    QJsonObject parameterSchema() const override;
    bool validate(const QJsonObject& params) const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    AgentScheduler* m_scheduler = nullptr;
};

class ScheduleListTool : public AITool {
public:
    explicit ScheduleListTool(AgentScheduler* scheduler);

    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    AgentScheduler* m_scheduler = nullptr;
};

class ScheduleCancelTool : public AITool {
public:
    explicit ScheduleCancelTool(AgentScheduler* scheduler);

    QJsonObject parameterSchema() const override;
    bool validate(const QJsonObject& params) const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    AgentScheduler* m_scheduler = nullptr;
};

class ScheduleSnoozeTool : public AITool {
public:
    explicit ScheduleSnoozeTool(AgentScheduler* scheduler);

    QJsonObject parameterSchema() const override;
    bool validate(const QJsonObject& params) const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    AgentScheduler* m_scheduler = nullptr;
};

#endif // DESKTOP_PET_SCHEDULE_TOOLS_H
