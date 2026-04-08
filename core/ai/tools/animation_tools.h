//
// Created by Huang_cj on 2026/4/8.
// 动画相关 Tools
// 封装 AnimationPlayer 的 API 给 AI 调用
//

#ifndef DESKTOP_PET_ANIMATION_TOOLS_H
#define DESKTOP_PET_ANIMATION_TOOLS_H

#include "../ai_tool.h"
#include "animation/animation_player.h"

// ================================================================
// Tool: play_animation
// 功能: 切换桌宠的动画状态
// 封装: AnimationPlayer::changeState()
// ================================================================
class PlayAnimationTool : public AITool {
public:
    // 构造时注入 AnimationPlayer 指针
    // 注意：这里不拥有 player 的所有权，调用方保证 player 的生命周期
    explicit PlayAnimationTool(AnimationPlayer* player)
        : AITool(
            "play_animation",
            "切换桌宠的动画状态。可选状态包括: Idle(待机), Dance(跳舞), "
            "Sitting(坐下), Sleeping(睡觉), Happy(开心), Cry(哭泣), "
            "Angry(生气), Fear(害怕), Eat(吃东西), Drink(喝水), "
            "Talk(说话), Hide(躲起来)。"
            "非循环动画播完后会自动回到 Idle 状态。",
            ToolCategory::Action
          )
        , m_player(player) {}

    QJsonObject parameterSchema() const override {
        // 构建合法的状态名枚举
        QJsonArray stateEnum;
        for (const auto& s : {"Idle", "Dance", "Sitting", "Sleeping",
             "Happy", "Cry", "Angry", "Fear", "Eat", "Drink",
             "Talk", "Hide", "BigScreen", "WindowSit"}) {
            stateEnum.append(s);
        }

        QJsonObject stateProp;
        stateProp["type"] = "string";
        stateProp["description"] = "目标动画状态名称";
        stateProp["enum"] = stateEnum;

        QJsonObject blendProp;
        blendProp["type"] = "number";
        blendProp["description"] = "过渡混合时长(秒)，默认 0.2";

        QJsonObject properties;
        properties["state"] = stateProp;
        properties["blend_duration"] = blendProp;

        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = properties;
        schema["required"] = QJsonArray{"state"};

        return schema;
    }

    ToolResult execute(const QJsonObject& params) override {
        if (!m_player) {
            return ToolResult::fail("AnimationPlayer not available");
        }

        // 1. 从 JSON 解析参数
        std::string targetState = params["state"].toString().toStdString();
        double blendDuration = params.value("blend_duration").toDouble(0.2);

        // 2. 记住切换前的状态（用于返回信息）
        std::string previousState = m_player->getCurrentStateName();

        // 3. 调用现有 API
        m_player->changeState(targetState, blendDuration);

        // 4. 构造结果返回给 LLM
        QJsonObject result;
        result["previous_state"] = QString::fromStdString(previousState);
        result["current_state"] = QString::fromStdString(targetState);
        result["blend_duration"] = blendDuration;

        return ToolResult::ok(result);
    }

private:
    AnimationPlayer* m_player = nullptr;  // 不拥有所有权
};

// ================================================================
// Tool: get_current_animation
// 功能: 查询当前动画状态（查询类，无副作用）
// 封装: AnimationPlayer::getCurrentStateName() + getCurrentClipName()
// ================================================================
class GetCurrentAnimationTool : public AITool {
public:
    explicit GetCurrentAnimationTool(AnimationPlayer* player)
        : AITool(
            "get_current_animation",
            "获取桌宠当前正在播放的动画状态和动画片段名称",
            ToolCategory::Query
          )
        , m_player(player) {}

    QJsonObject parameterSchema() const override {
        // 无参数
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        if (!m_player) {
            return ToolResult::fail("AnimationPlayer not available");
        }

        QJsonObject result;
        result["state"] = QString::fromStdString(m_player->getCurrentStateName());
        result["clip"] = QString::fromStdString(m_player->getCurrentClipName());

        return ToolResult::ok(result);
    }

private:
    AnimationPlayer* m_player = nullptr;
};

#endif // DESKTOP_PET_ANIMATION_TOOLS_H