//
// AIBrain
// 负责 think 循环、上下文构建、LLM 请求和 tool 调度
//

#ifndef DESKTOP_PET_AI_BRAIN_H
#define DESKTOP_PET_AI_BRAIN_H

#include <QObject>
#include <QHash>
#include <QTimer>
#include <QElapsedTimer>
#include <QList>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "agent/agent_session.h"
#include "ai_call_logger.h"
#include "ai_types.h"
#include "chat/chat_side_effect_queue.h"
#include "chat/chat_types.h"
#include "domain/domain_result.h"
#include "emotion/emotion_types.h"
#include "event/event_types.h"
#include "identity/identity_baseline.h"
#include "llm/llm_chat_model_client.h"
#include "llm/llm_chat_service.h"
#include "memory/daydream_consolidator.h"
#include "memory/memory_extractor.h"
#include "memory/memory_policy.h"
#include "memory/memory_retriever.h"
#include "memory/memory_store.h"
#include "memory/working_memory_cache.h"
#include "model/model_role_registry.h"
#include "model/model_router.h"
#include "prompt/prompt_template_types.h"
#include "reflection/cancellation_token.h"
#include "runtime/runtime_types.h"
#include "scheduler/daydream_trigger_policy.h"
#include "scheduler/opportunity_types.h"
#include "scheduler/proactive_chat_policy.h"
#include "task/task_types.h"

class AgentScheduler;
class AgentRuntimeServices;
class ChatPreparationExecutor;
struct ChatPreparationEnvironment;
struct ChatPreparationRequest;
struct ChatPreparationResult;
#include "router/intent_router.h"
#include "skill/skill_store.h"
#include "tool_registry.h"
#include "chat/conversation_types.h"
#include "tools/runtime/tool_runtime.h"

class MemoryConsolidationService;
class TaskStore;
class ActivityRuntime;
class TaskOrchestrator;
class TaskToolExecutor;
class TaskExperienceAdapter;

class PluginRuntime;
class AIBrain : public QObject {
    Q_OBJECT

    friend class TestModuleConnectivity;

public:
    using EmotionSnapshotProvider = std::function<std::optional<EmotionSnapshot>()>;

    explicit AIBrain(QObject* parent = nullptr);
    AIBrain(ModelCompletionClient* modelClient,
            QList<ModelRoleConfig> modelRoles,
            QObject* parent = nullptr);
    ~AIBrain() override;

    Result<void, DomainError> initializeStorage(const AIBrainStorageConfig& config);
    bool isStorageInitialized() const { return m_storageInitialized; }
    void setRuntimeServices(AgentRuntimeServices* services);

    Result<void, DomainError> registerConversationBinding(const HostConversationBinding& binding);
    void submitIncomingMessage(IncomingMessage message,
        std::function<void(Result<AcceptedMessage, DomainError>)> completion = {});
    void setPetName(const QString& petName);
    void setToolRegistry(ToolRegistry* registry);
    PluginRuntime* pluginRuntime() const { return m_pluginRuntime.get(); }
    void setAgentScheduler(AgentScheduler* scheduler); // non-owning，供 Daydream 距待办判定
    void setEmotionSnapshotProvider(EmotionSnapshotProvider provider);

    // 注入通用提示词模版与独立身份基线（由聊天准备线程构建上下文）。
    void setIdentityBaseline(const IdentityBaseline& baseline);
    void setAvatarContextProvider(std::function<QJsonObject()> provider) { m_avatarContextProvider = std::move(provider); }
    void setPromptTemplate(const PromptTemplate& templ);
#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
    void setChatPreparationDelayForTests(int delayMs);
    void setChatSideEffectDelayForTests(int delayMs);
    void setChatSideEffectLifecycleProbeForTests(
        ChatSideEffectQueue::LifecycleProbe probe);
#endif

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    void setThinkIntervalMs(int ms);

    bool isBusy() const { return m_busy; }
    bool canStartProactiveChat() const;
    bool autonomyEnabled() const {
        return m_autonomyConfig.enabled;
    }
    void setAutonomyConfig(const AutonomyConfig &);
    void setAutonomyPaused(bool paused);
    void requestAutonomousOpportunity(const QString &kind = "tick", const QString &contextId = {},
                                      const QStringList &eventIds = {});
    ProactiveChatTiming proactiveChatTiming(int baseIntervalMs) const;
    QString proactiveStatePath() const { return m_proactiveStatePath; }
    Result<bool, DomainError> requestManualDaydream();
    bool canAcceptUserMessage() const;
    Result<void, DomainError> submitUserMessage(const QString& text,
                                               const QString& messageId,
                                               bool rememberInput = true);
    int pendingUserMessageCount() const { return m_pendingUserMessages.size(); }
    QStringList activeUserMessageIds() const;
    quint64 interactionRevision() const { return m_interactionRevision; }
    int userIdleSeconds() const;
    ModelRoleRegistry* modelRoleRegistry() { return &m_modelRoleRegistry; }
    ModelRouter* modelRouter() { return &m_modelRouter; }
    MemoryConsolidationService* consolidationService() { return m_consolidationService.get(); }

    void start();
    void stop();

    void triggerThink(const QString& reason = "manual",
                      const QString& triggerTag = "manual",
                      const QString& replyToId = {},
                      const QString& voiceSource = {});
    void stopCurrentResponse();
    QJsonObject taskSnapshot() const;
    bool resumeTask(const QString& feedback = {}, const TaskBudgetExtension& extension = {});
    void pauseTask();
    void stopTask();
    bool resolveTaskUnknownCall(const QString& callId, bool succeeded);
    bool isTaskActive() const;
    void onUserInteraction(const QString& eventName, const QString& detail = QString());

    void clearMemory();
    void restoreConversationHistory(const QList<ChatHistoryEntry>& history);
    void rememberScreenObservation(const QString& observation);
    void resolveToolConfirmation(const QString& requestId, bool approved);
    MemoryStore* memoryStore() { return &m_memoryStore; }
    const MemoryStore* memoryStore() const { return &m_memoryStore; }
    SkillStore* skillStore() { return &m_skillStore; }
    const SkillStore* skillStore() const { return &m_skillStore; }

signals:
    void taskProgressChanged(const QJsonObject& task);
    void pendingUserMessagesChanged();
    void thinkingStarted(const QString& reason);
    void thinkingFinished(bool success, const QString& errorMessage);
    void thinkRequestRejected(const QString& replyToId,
                              const QString& errorMessage);
    void assistantResponseReady(const QString& content, const QString& voiceSource);
    void assistantResponseStarted(const QString& messageId,
                                  const QString& replyToId,
                                  const QString& triggerTag);
    void assistantResponseStageChanged(const QString& messageId,
                                       ChatActivityStage stage);
    void assistantResponseDelta(const QString& messageId,
                                const QString& textDelta);
    void assistantResponseFinished(const QString& messageId,
                                   ChatMessageStatus status,
                                   const QString& errorMessage);
    void toolExecuted(const QString& toolName, bool success, const QString& payload);
    void toolConfirmationRequired(const QString& requestId,
                                  const QString& toolName,
                                  const QString& reason,
                                  const QJsonObject& arguments);
    void daydreamStarted(int itemCount);
    void daydreamFinished(const QJsonObject& summary);
    void daydreamCancelled(const QString& reason);
    void chatPreparationTimingsObserved(ChatPreparationTimings timings);

private:
    void initializeTaskRuntime();
    void refreshTaskDependencies();
    void initializePluginRuntime();
    void retirePluginRuntime();
    ContextSpec currentToolContext() const;
    void shutdownTaskRuntime();
    bool tryHandleComplexTask(const QString& text, const QString& triggerTag);
    void finishTaskResponse();
    std::unique_ptr<ActivityRuntime> m_activityRuntime;
    bool m_taskCommandPending=false;
    QSet<QString> m_suspendedTaskIds;
    std::unique_ptr<TaskToolExecutor> m_taskToolExecutor;
    std::unique_ptr<TaskExperienceAdapter> m_taskExperienceAdapter;
    QHash<QString, std::shared_ptr<LlmRequestHandle>> m_taskModelRequests;
    bool m_taskResponse = false;
    QTimer m_taskExperienceTimer;
    struct PendingUserMessage {
        QString text;
        QString id;
        QDateTime receivedAt;
        bool rememberInput = true;
        std::optional<AcceptedMessage> accepted;
        std::optional<IncomingMessage> original;
        qint64 queuedAtMonotonicMs = 0;
    };
    void schedulePendingUserMessages();
    void dispatchPendingUserMessages();
    void cancelActiveResponse();
    QTimer m_userMessageDispatchTimer;
    QList<PendingUserMessage> m_pendingUserMessages;
    QList<PendingUserMessage> m_dispatchingUserMessages;
    int m_acceptingMessageCount = 0;
    quint64 m_inputEpoch = 0;
    QSet<QString> m_scheduledMessageIds;
    QString m_currentConversationId = QStringLiteral("local_owner");
    QHash<QString, QList<ChatMessage>> m_conversationWindows;
    QString m_proactiveStatePath;
    static bool isAutomaticTrigger(const QString& triggerTag);
    bool automaticTextAllowed() const;
    RuntimeReadiness autonomousReadiness() const;
    AutonomyConfig m_autonomyConfig;
    bool m_autonomyPaused = false;
    quint64 m_opportunityHostRevision = 0;
    QByteArray m_opportunityReadinessKey;
    QJsonObject m_opportunityInputWatermarks;
    bool suppressMutedAutomaticResponse();
    void thinkInternal(const QString& reason,
                      const QString& triggerTag,
                      const QString& sessionId,
                      int toolRound,
                      const QList<ChatMessage>& workingMessages);
    void continuePreparedThink(ChatPreparationResult result);
    ChatPreparationRequest makeChatPreparationRequest(
        const QString& requestId,
        quint64 generation,
        const QString& reason,
        const QString& triggerTag,
        const QString& sessionId) const;
    QString beginPreparationRuntimeSession(const QString& reason,
                                           const QString& triggerTag);
    Result<void, DomainError> startChatPreparationExecutor();
    Result<void, DomainError> startChatSideEffectQueue();
    void handleSideEffectBarrier(const QString& sessionId, quint64 generation);
    void enqueueCallLog(ChatSideEffectType type,
                        const QString& requestId,
                        const QString& sessionId,
                        quint64 generation,
                        QJsonObject record);

    bool tryHandleRoutedIntent(const IntentRoute& route,
                               const QString& reason,
                               const QString& triggerTag,
                               const QString& sessionId);
    bool bindLocalRuntimeSnapshot(const QString& sessionId);
    bool shouldUseLocalRouter(const QString& triggerTag) const;
    ToolPolicyContext buildToolPolicyContext(const QString& triggerTag,
                                             const QString& userInput,
                                             bool initiatedByLlm) const;
    void rememberToolOutcome(const QString& toolName,
                             const QString& triggerTag,
                             bool initiatedByLlm,
                             const ToolExecutionOutcome& outcome,
                             const QString& sessionId = QString());
    void enqueueUserMemoryWriteForSource(const QString& input, const QString& triggerTag,
        const QString& requestId, quint64 generation, const QString& sessionId,
        const std::optional<AcceptedMessage>& source, const QDateTime& originalTime);
    void enqueueUserMemoryWrite(const QString& input,
                                const QString& triggerTag,
                                const QString& requestId,
                                quint64 generation,
                                const QString& sessionId);
    std::optional<EmotionSnapshot> currentEmotionSnapshot() const;
    void annotateMemoryEntry(MemoryEntry& entry) const;
    void appendToMemory(const ChatMessage& message);
    bool appendRuntimeEvent(const QString& type,
                            const QString& sessionId,
                            const QJsonObject& payload,
                            const QString& requestId = {},
                            quint64 generation = 0);
    void finishRuntimeSession(const QString& sessionId, quint64 generation);
    void setupTriggerTimers();
    void scheduleTrigger(const QString& triggerTag);
    void beginActiveResponse(const QString& replyToId,
                             const QString& triggerTag,
                             const QString& sessionId);
    void publishActiveStage(ChatActivityStage stage);
    void appendActiveDelta(const QString& textDelta);
    void finishActiveResponse(ChatMessageStatus status,
                              const QString& errorMessage = {},
                              const QJsonObject& responseLog = {});
    // Background memory maintenance shares the existing writer and model executor.
    void checkDaydreamTrigger();
    void runDaydreamSession();
    void cancelDaydreamSession(const QString& reason);
    void armDaydreamTimer();
    AiTriggerConfig triggerConfigForTag(const QString& triggerTag) const;
    void armProactiveChatCheck();
    QStringList allowedActionsForTrigger(const QString& triggerTag) const;
    bool isToolCallAllowed(const QString& triggerTag,
                           const LlmToolCall& call,
                           QString& denialReason) const;
    void scheduleIdleRetryIfBusyFailure(const QString& toolName,
                                        const QString& toolPayload);
    static QList<ModelRoleConfig> configuredModelRoles();

private:
    QString m_petName;
    std::unique_ptr<PluginRuntime> m_pluginRuntime;
    ToolRegistry* m_toolRegistry = nullptr; // non-owning
    AgentRuntimeServices* m_runtimeServices = nullptr; // non-owning

    std::function<QJsonObject()> m_avatarContextProvider;
    IdentityBaseline m_identityBaseline = IdentityBaseline::defaults();
    PersonalityPolicy m_personalityPolicy;
    PromptTemplate m_promptTemplate;
    std::unique_ptr<ChatPreparationExecutor> m_chatPreparationExecutor;
    std::unique_ptr<ChatSideEffectQueue> m_chatSideEffectQueue;
    std::unique_ptr<ChatPreparationEnvironment> m_chatPreparationEnvironment;
    ChatPreparationRuntimeMetadata m_chatPreparationRuntimeMetadata;
    LlmChatService m_chatService;
    ModelRoleRegistry m_modelRoleRegistry{configuredModelRoles()};
    LlmChatModelClient m_modelClient{&m_chatService};
    ModelRouter m_modelRouter;
    IntentRouter m_intentRouter;
    ToolRuntime m_toolRuntime;
    MemoryStore m_memoryStore;
    MemoryExtractor m_memoryExtractor;
    MemoryPolicy m_memoryPolicy;
    WorkingMemoryCache m_workingMemoryCache;
    SkillStore m_skillStore;

    QTimer m_idleTriggerTimer;
    QTimer m_chatTriggerTimer;
    QTimer m_daydreamTimer;
    DaydreamConfig m_daydreamConfig;
    DaydreamTriggerPolicy m_daydreamPolicy;
    AgentScheduler* m_scheduler = nullptr; // non-owning
    bool m_daydreamRunning = false;
    QDateTime m_lastDaydreamAt;
    QDateTime m_daydreamHourAnchor;
    int m_daydreamCountThisHour = 0;
    QSet<QString> m_pendingMemoryRefreshIds;
    CancellationSource m_daydreamCancellation;
    std::unique_ptr<MemoryConsolidationService> m_consolidationService;

    bool m_enabled = true;
    bool m_storageInitializationAttempted = false;
    bool m_storageInitialized = false;
    bool m_running = false;
    bool m_busy = false;
    bool m_idleRetryScheduled = false;
    quint64 m_requestGeneration = 0;
    quint64 m_interactionRevision = 0;
    QElapsedTimer m_conversationCooldown;
    bool m_cooldownAfterProactive = false;
    int m_unansweredProactiveChats = 0;
    QElapsedTimer m_proactiveOpportunityClock;
    int m_proactiveBaseIntervalMs = 180000;

    struct ActiveDialogueResponse {
        QString messageId;
        std::optional<ContextSpec> context;
        std::optional<ReplyTarget> replyTarget;
        QList<PendingUserMessage> originalMessages;
        bool publishing = false;
        bool publicationFinished = false;
        QString replyToId;
        QString triggerTag;
        QString sessionId;
        QString reason;
        QString voiceSource;
        QString preparationRequestId;
        QString inputEventId;
        std::optional<EventDraft> inputEventDraft;
        QStringList reinforcementIds;
        QStringList sourceMessageIds;
        QDateTime inputMentionedAt;
        bool rememberInput = true;
        QString visibleContent;
        QList<ChatMessage> priorConversation;
        ChatMessageStatus status = ChatMessageStatus::Pending;
        ChatActivityStage stage = ChatActivityStage::WaitingForModel;
        quint64 generation = 0;
        qint64 acceptedAtMonotonicMs = 0;
        qint64 preparedAtMonotonicMs = 0;
        qint64 uiAcknowledgeMs = 0;
        qint64 preparationMs = 0;
        std::shared_ptr<LlmRequestHandle> requestHandle;
        bool terminal = false;
        bool announced = false;
    };
    std::optional<ActiveDialogueResponse> m_activeDialogueResponse;

    int m_maxToolRounds = 3;
    int m_maxMemoryMessages = 20;

    QList<ChatMessage> m_memory;
    QHash<QString, AgentSession> m_runtimeSessions;
    QHash<QString, quint64> m_pendingSideEffectBarriers;
    struct PendingResponseLog {
        QString sessionId;
        quint64 generation = 0;
        std::shared_ptr<std::atomic_bool> handled;
    };
    QHash<QString, PendingResponseLog> m_pendingResponseLogs;
    QHash<QString, std::function<void(bool)>> m_pendingToolConfirmations;
    AiCallLogger m_callLogger;
    EmotionSnapshotProvider m_emotionSnapshotProvider;
};

#endif // DESKTOP_PET_AI_BRAIN_H
