//
// Created by Huang_cj on 2026/4/8.
// 动画相关 Tools
// 封装 AnimationPlayer 的 API 给 AI 调用
//

#ifndef DESKTOP_PET_ANIMATION_TOOLS_H
#define DESKTOP_PET_ANIMATION_TOOLS_H

#include "../ai_tool.h"
#include "animation/animation_player.h"
#include "animation/animation_manager.h"

#include <QJsonArray>

#include "configLoader/config_manager.h"

namespace {
inline bool isIdleState(const QString& stateName) {
    return stateName.compare("Idle", Qt::CaseInsensitive) == 0;
}

constexpr double kLocalBlendDurationSeconds = 0.2;
}

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

        QJsonObject properties;
        properties["state"] = stateProp;

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

        const QString currentState = QString::fromStdString(m_player->getCurrentStateName());
        if (!isIdleState(currentState)) {
            return ToolResult::fail(
                QString("Current state '%1' is busy. Action transitions are only allowed in Idle.")
                    .arg(currentState));
        }

        // 1. 从 JSON 解析参数
        std::string targetState = params["state"].toString().toStdString();
        constexpr double blendDuration = kLocalBlendDurationSeconds;

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

// ================================================================
// Tool: get_idle_transition_candidates
// 功能: 查询当前可用于空闲调度的动作候选列表
// 封装: AnimationManager::getStateMachine() + AnimationPlayer::getCurrentStateName()
// ================================================================
class GetIdleTransitionCandidatesTool : public AITool {
public:
    GetIdleTransitionCandidatesTool(AnimationPlayer* player, const AnimationManager* animationManager)
        : AITool(
            "get_idle_transition_candidates",
            "查询当前可用于空闲动作切换的候选状态列表。仅返回可播放且符合本地策略的状态。",
            ToolCategory::Query
          )
        , m_player(player)
        , m_animationManager(animationManager) {}

    QJsonObject parameterSchema() const override {
        QJsonObject limitProp;
        limitProp["type"] = "integer";
        limitProp["description"] = "最多返回多少个候选动作，默认 12";

        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{{"limit", limitProp}};
        return schema;
    }

    ToolResult execute(const QJsonObject& params) override {
        if (!m_player || !m_animationManager) {
            return ToolResult::fail("Animation system not available");
        }

        int limit = params.value("limit").toInt(12);
        if (limit <= 0) limit = 12;

        const auto& stateMachine = m_animationManager->getStateMachine();
        const AiBehaviorPolicy& policy = ConfigManager::instance().getAiBehaviorPolicy();
        const std::string current = m_player->getCurrentStateName();

        QJsonArray candidates;
        for (const auto& state : stateMachine.states) {
            const QString stateName = QString::fromStdString(state.name);

            if (state.clipOptions.empty()) {
                continue;
            }
            if (stateName.startsWith("Touch", Qt::CaseInsensitive)) {
                continue;
            }
            if (policy.forbiddenActions.contains(stateName, Qt::CaseInsensitive)) {
                continue;
            }

            candidates.append(stateName);
            if (candidates.size() >= limit) {
                break;
            }
        }

        QJsonArray preferred;
        auto transIt = stateMachine.transactionMap.find(current);
        if (transIt != stateMachine.transactionMap.end()) {
            for (int transitionIndex : transIt->second) {
                if (transitionIndex < 0 || transitionIndex >= static_cast<int>(stateMachine.transactions.size())) {
                    continue;
                }
                const auto& transition = stateMachine.transactions[transitionIndex];
                const QString toState = QString::fromStdString(transition.toState);
                if (toState.startsWith("Touch", Qt::CaseInsensitive)) {
                    continue;
                }
                if (policy.forbiddenActions.contains(toState, Qt::CaseInsensitive)) {
                    continue;
                }
                preferred.append(toState);
            }
        }

        QJsonObject result;
        result["current_state"] = QString::fromStdString(current);
        result["candidates"] = candidates;
        result["preferred_next"] = preferred;
        return ToolResult::ok(result);
    }

private:
    AnimationPlayer* m_player = nullptr;
    const AnimationManager* m_animationManager = nullptr;
};

// ================================================================
// Tool: get_action_transition_status
// 功能: 获取动作切换相关运行状态
// 封装: AnimationPlayer 查询接口
// ================================================================
class GetActionTransitionStatusTool : public AITool {
public:
    explicit GetActionTransitionStatusTool(AnimationPlayer* player)
        : AITool(
            "get_action_transition_status",
            "获取当前动作状态、片段、播放时间和是否正在混合过渡。",
            ToolCategory::Query
          )
        , m_player(player) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        if (!m_player) {
            return ToolResult::fail("AnimationPlayer not available");
        }

        const QString state = QString::fromStdString(m_player->getCurrentStateName());
        const bool isIdle = isIdleState(state);

        QJsonObject result;
        result["current_state"] = state;
        result["current_clip"] = QString::fromStdString(m_player->getCurrentClipName());
        result["state_time_seconds"] = m_player->getCurrentTimeSeconds();
        result["is_cross_fading"] = m_player->isCrossFading();
        result["is_touch_reaction"] = state.startsWith("Touch", Qt::CaseInsensitive);
        result["is_idle"] = isIdle;
        result["is_busy"] = !isIdle;
        return ToolResult::ok(result);
    }

private:
    AnimationPlayer* m_player = nullptr;
};

// ================================================================
// Tool: request_idle_transition
// 功能: 请求切换空闲动作（本地校验后执行）
// 封装: AnimationPlayer::changeState()
// ================================================================
class RequestIdleTransitionTool : public AITool {
public:
    RequestIdleTransitionTool(AnimationPlayer* player, const AnimationManager* animationManager)
        : AITool(
            "request_idle_transition",
            "请求切换到一个空闲动作状态。本地会做候选合法性、禁用动作和触摸反应互斥检查。",
            ToolCategory::Action
          )
        , m_player(player)
        , m_animationManager(animationManager) {}

    QJsonObject parameterSchema() const override {
        QJsonObject targetProp;
        targetProp["type"] = "string";
        targetProp["description"] = "目标动作状态名";

        QJsonObject reasonProp;
        reasonProp["type"] = "string";
        reasonProp["description"] = "切换理由，便于日志追踪";

        QJsonObject properties;
        properties["target_action"] = targetProp;
        properties["reason"] = reasonProp;

        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = properties;
        schema["required"] = QJsonArray{"target_action"};
        return schema;
    }

    ToolResult execute(const QJsonObject& params) override {
        if (!m_player || !m_animationManager) {
            return ToolResult::fail("Animation system not available");
        }

        const QString target = params.value("target_action").toString().trimmed();
        const QString current = QString::fromStdString(m_player->getCurrentStateName());
        constexpr double blendDuration = kLocalBlendDurationSeconds;

        if (target.isEmpty()) {
            return ToolResult::fail("target_action is required");
        }

        if (!isIdleState(current)) {
            return ToolResult::fail(
                QString("Current state '%1' is busy. request_idle_transition is only allowed in Idle.")
                    .arg(current));
        }

        if (current.startsWith("Touch", Qt::CaseInsensitive)) {
            return ToolResult::fail("Touch reaction is playing, idle transition denied");
        }

        if (target.startsWith("Touch", Qt::CaseInsensitive)) {
            return ToolResult::fail("Touch actions are managed by local interaction pipeline");
        }

        const AiBehaviorPolicy& policy = ConfigManager::instance().getAiBehaviorPolicy();
        if (policy.forbiddenActions.contains(target, Qt::CaseInsensitive)) {
            return ToolResult::fail(QString("Action '%1' is forbidden by policy").arg(target));
        }

        const auto& stateMachine = m_animationManager->getStateMachine();
        const std::string targetStd = target.toStdString();
        auto stateIt = stateMachine.stateIndexMap.find(targetStd);
        if (stateIt == stateMachine.stateIndexMap.end()) {
            return ToolResult::fail(QString("State '%1' not found").arg(target));
        }

        const auto& state = stateMachine.states[stateIt->second];
        if (state.clipOptions.empty()) {
            return ToolResult::fail(QString("State '%1' has no playable clips").arg(target));
        }

        m_player->changeState(targetStd, blendDuration);

        QJsonObject result;
        result["approved"] = true;
        result["previous_state"] = current;
        result["current_state"] = QString::fromStdString(m_player->getCurrentStateName());
        result["requested_action"] = target;
        result["blend_duration"] = blendDuration;
        result["reason"] = params.value("reason").toString();
        return ToolResult::ok(result);
    }

private:
    AnimationPlayer* m_player = nullptr;
    const AnimationManager* m_animationManager = nullptr;
};

#endif // DESKTOP_PET_ANIMATION_TOOLS_H