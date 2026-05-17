//
// Created by Inoriac on 2026/4/8.
//

#ifndef DESKTOP_PET_AI_TYPES_H
#define DESKTOP_PET_AI_TYPES_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QList>
#include <QStringList>

// tool的类别标记
enum class ToolCategory {
    Query,      // 只读查询
    Action      // 有副作用的操作
};

struct ToolResult {
    bool success = false;
    QJsonObject data;
    QString errorMessage;

    // 快捷构造：成功
    static ToolResult ok(const QJsonObject& resultData = {}) {
        return {true, resultData, ""};
    }

    // 快捷构造：成功
    static ToolResult fail(const QString& error) {
        return {false, {}, error};
    }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["success"] = success;
        if (success) {
            obj["data"] = data;
        } else {
            obj["error"] = errorMessage;
        }
        return obj;
    }
};

// LLM 连接与推理配置
struct LlmConfig {
    bool enabled = false;
    QString provider = "openai-compatible";
    QString baseUrl = "https://api.openai.com/v1";
    QString apiKey;
    QString model = "gpt-4o-mini";
    QString visualModel = "Qwen/Qwen3-VL-32B-Thinking";

    int timeoutMs = 30000;
    int maxTokens = 512;
    double temperature = 0.7;
    int retryCount = 1;
    int thinkIntervalMs = 30000;

    // 为兼容不同网关保留扩展参数。
    QJsonObject extraParams;
};

// 屏幕识别对话配置
struct ScreenChatConfig {
    bool enabled = false;
    int minIntervalMs = 8 * 60 * 1000;
    int maxIntervalMs = 12 * 60 * 1000;

    // 气泡样式和布局
    int bubbleOpacityPercent = 80;
    int bubbleFontSize = 14;
    int bubbleOffsetX = 0;
    int bubbleOffsetY = -20;
    int bubbleDurationMs = 8000;

    QString petGender = "female";
};

// 本地音乐控制配置（当前仅 Windows 网易云）
struct MusicControlConfig {
    bool enabled = false;
    QString provider = "netease_windows";
    QString clientPath;
    QString serviceBaseUrl = "http://127.0.0.1:5010";
    int requestTimeoutMs = 3000;
    QString concurrencyPolicy = "replace";
};

// 单个触发器的调度配置
struct AiTriggerConfig {
    bool enabled = true;
    int minIntervalMs = 60000;
    int maxIntervalMs = 120000;
};

// AI 行为策略：白名单 + 禁用动作 + 触发器频率
struct AiBehaviorPolicy {
    QStringList idleActionWhitelist;
    QStringList touchActionWhitelist;
    QStringList emotionActionWhitelist;
    QStringList forbiddenActions;

    AiTriggerConfig idleTrigger;
    AiTriggerConfig emotionTrigger;
    AiTriggerConfig proactiveChatTrigger;
};

// 发送给 LLM 的单条消息
struct ChatMessage {
    QString role;       // system / user / assistant / tool
    QString content;
    QString name;       // 可选
    QString toolCallId; // tool 角色消息使用
    QJsonArray toolCalls; // assistant 角色在 function calling 场景下使用
};

// LLM 返回的单个 tool_call
struct LlmToolCall {
    QString id;
    QString type;   // 通常为 function
    QString name;   // function name
    QJsonObject arguments;
};

// 单次 LLM 调用的 token 使用情况
struct LlmUsage {
    qint64 promptTokens = 0;
    qint64 completionTokens = 0;
    qint64 totalTokens = 0;

    qint64 reasoningTokens = 0;
    qint64 cachedTokens = 0;
    qint64 promptCacheHitTokens = 0;
    qint64 promptCacheMissTokens = 0;
};

// 单次 completion 解析结果
struct LlmResponse {
    QString id;
    QString model;
    qint64 created = 0;

    QString content;
    QString reasoningContent;
    QString finishReason;
    LlmUsage usage;
    QList<LlmToolCall> toolCalls;
};

#endif //DESKTOP_PET_AI_TYPES_H