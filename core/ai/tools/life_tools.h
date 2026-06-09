//
// 生活助理工具
//

#ifndef DESKTOP_PET_LIFE_TOOLS_H
#define DESKTOP_PET_LIFE_TOOLS_H

#include "../ai_tool.h"

class WeatherQueryTool : public AITool {
public:
    WeatherQueryTool();

    QJsonObject parameterSchema() const override;
    bool validate(const QJsonObject& params) const override;
    ToolResult execute(const QJsonObject& params) override;
};

class HolidayQueryTool : public AITool {
public:
    HolidayQueryTool();

    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;
};

class DailyBriefingTool : public AITool {
public:
    DailyBriefingTool();

    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;
};

#endif // DESKTOP_PET_LIFE_TOOLS_H
