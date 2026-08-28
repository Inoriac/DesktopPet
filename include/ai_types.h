//
// Created by Inoriac on 2026/4/8.
//

#ifndef DESKTOP_PET_AI_TYPES_H
#define DESKTOP_PET_AI_TYPES_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QTime>
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

    QString anthropicVersion = "2023-06-01";
    QJsonObject extraHeaders;

    // 为兼容不同网关保留扩展参数。
    QJsonObject extraParams;
};

enum class ModelRole {
    Dialogue,
    FastExtract,
    Consolidation,
    Diary,
    Vision
};

enum class ContextPartition {
    CurrentInput,
    Persona,
    RelevantMemory,
    SkillSummary,
    EvidenceWindow,
    DiaryProjection,
    InnerThought,
    VisionInput,
    OwnerAccess
};

struct ModelLimits {
    int maxLatencyMs = 0;
    qint64 maxEstimatedCostMicros = 0;
};

struct ModelRouteConfig {
    QString routeId;
    bool enabled = true;
    LlmConfig llm;
    bool supportsVision = false;
    qint64 estimatedCostMicros = 0;
};

struct ModelRoleConfig {
    ModelRole role = ModelRole::Dialogue;
    QList<ModelRouteConfig> routes;
    ModelLimits limits;
};

struct ModelConstraints {
    int maxLatencyMs = 0;
    qint64 maxEstimatedCostMicros = 0;
    bool requiresVision = false;
};

struct LlmCallDimensions {
    ModelRole role = ModelRole::Dialogue;
    QString provider;
    QString model;
    QString routeId;
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

// 空闲记忆整理配置。模型留空时复用主 LLM 配置中的 model。
struct DaydreamConfig {
    bool enabled = true;
    int idleThresholdSec = 5 * 60;
    int dueSoonThresholdMs = 10 * 60 * 1000;
    int minIntervalMs = 15 * 60 * 1000;
    int interruptionBackoffMs = 10 * 60 * 1000;
    int hourlyLimit = 3;
    int tickIntervalMs = 30 * 1000;

    int sessionLimit = 32;
    int batchLimit = 8;
    int inboxLimit = 200;
    int relatedMemoryLimit = 8;

    QString model;
    int maxTokens = 1200;
    double temperature = 0.2;
};

struct SleepPolicy {
    bool enabled = true;
    QTime bedtime = QTime(23, 30);
    int minimumIdleSeconds = 600;
    int dueSoonThresholdSeconds = 600;
    int maxItemsPerSession = 32;
    int retryBackoffSeconds = 600;
    int tickIntervalSeconds = 60;
};

// 自定义语音角色配置
struct CustomVoiceConfig {
    QString name;
    QString language = "zh";
    QString onnxModelDir;
    QString referenceAudioPath;
    QString referenceAudioText;
};

// 语音播报来源开关
struct VoiceSourceConfig {
    bool assistant = true;
    bool proactive = true;
    bool screenChat = true;
    bool fallback = true;
    bool toolBubble = false;
};

// Python / GENIE 语音合成配置
struct VoiceConfig {
    bool enabled = false;
    QString backend = "genie-tts";
    QString pythonExecutable;
    QString venvPath = ".venv";
    QString workerScript = "tools/voice/genie_worker.py";
    bool preloadOnStart = true;
    bool allowAutoDownload = false;

    QString genieDataDir = "runtime/voice/GenieData";
    QString characterModelsDir = "runtime/voice/CharacterModels";
    QString customCharactersDir = "runtime/voice/custom_characters";

    QString speakerMode = "predefined";
    QString selectedSpeaker = "feibi";
    CustomVoiceConfig customSpeaker;

    bool saveAudio = false;
    QString outputDir = "runtime/voice/outputs";
    VoiceSourceConfig sources;
    int maxTextChars = 350;
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

// AI 工具访问策略：限制文件根目录、文件写入和命令执行白名单。
struct AiToolAccessPolicy {
    QStringList allowedRoots;
    bool allowFileWrite = false;
    bool allowCommandExecution = false;
    QStringList commandWhitelist;
    QStringList autoGrantedTools;
    int commandTimeoutMs = 5000;
    int maxWriteBytes = 64 * 1024;
};

// 发送给 LLM 的单条消息
struct ChatMessage {
    QString role;       // system / user / assistant / tool
    QString content;
    QString name;       // 可选
    QString toolCallId; // tool 角色消息使用
    QJsonArray toolCalls; // assistant 角色在 function calling 场景下使用
    QJsonArray transportBlocks; // provider 原生块，仅在当前请求链内续传
    QJsonArray contentBlocks; // provider-neutral text/image blocks
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
    QJsonArray transportBlocks;
};

struct ModelRequest {
    ModelRole role = ModelRole::Dialogue;
    QList<ChatMessage> messages;
    QJsonArray tools;
    QJsonObject responseSchema;
    ModelConstraints constraints;
    QString profileId;
    QString sessionId;
    QString petName;
};

struct ModelCompletion {
    LlmResponse response;
    LlmCallDimensions dimensions;
    bool fallbackUsed = false;
};

struct ContextProjection {
    ContextPartition partition = ContextPartition::CurrentInput;
    QList<ChatMessage> messages;
};

struct ContextRequest {
    int queryBudgetChars = 12000;
    QList<ContextPartition> requestedPartitions;
    QList<ContextProjection> projections;
};

#endif //DESKTOP_PET_AI_TYPES_H
