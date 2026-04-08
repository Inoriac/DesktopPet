//
// Created by Huang_cj on 2026/4/8.
// 环境查询 Tools
// 提供时间等环境信息，不依赖任何项目内部系统
//

#ifndef DESKTOP_PET_ENVIRONMENT_TOOLS_H
#define DESKTOP_PET_ENVIRONMENT_TOOLS_H

#include "../ai_tool.h"
#include <QDateTime>
#include <QSysInfo>

// ================================================================
// Tool: get_current_time
// 功能: 获取当前时间和日期
// 无依赖：这个 Tool 不需要注入任何内部对象
// AI 可以用这个决定"现在是深夜，桌宠应该打哈欠"之类的行为
// ================================================================
class GetCurrentTimeTool : public AITool {
public:
    GetCurrentTimeTool()
        : AITool(
            "get_current_time",
            "获取当前的日期和时间信息，包括星期几、具体时刻、"
            "以及时间段描述（凌晨/上午/下午/晚上）。"
            "可以用来决定桌宠应该做符合当前时段的事情。",
            ToolCategory::Query
          ) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        QDateTime now = QDateTime::currentDateTime();

        QJsonObject result;
        result["datetime"] = now.toString("yyyy-MM-dd hh:mm:ss");
        result["date"] = now.toString("yyyy-MM-dd");
        result["time"] = now.toString("hh:mm");
        result["day_of_week"] = now.toString("dddd");  // 如 "星期三"
        result["hour"] = now.time().hour();

        // 给 LLM 一个人类可读的时段描述
        int hour = now.time().hour();
        QString period;
        if (hour < 6) period = "凌晨";
        else if (hour < 12) period = "上午";
        else if (hour < 14) period = "中午";
        else if (hour < 18) period = "下午";
        else if (hour < 22) period = "晚上";
        else period = "深夜";
        result["period"] = period;

        return ToolResult::ok(result);
    }
};

#endif // DESKTOP_PET_ENVIRONMENT_TOOLS_H