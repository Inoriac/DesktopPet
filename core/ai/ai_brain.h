//
// AIBrain
// 负责 think 循环、上下文构建、LLM 请求和 tool 调度
//

#ifndef DESKTOP_PET_AI_BRAIN_H
#define DESKTOP_PET_AI_BRAIN_H

#include <QObject>
#include <QHash>
#include <QTimer>
#include <QList>
#include <QStringList>
#include <functional>
#include <optional>

#include "ai_types.h"
#include "ai_call_logger.h"
#include "context_builder.h"
#include "emotion/emotion_types.h"
#include "llm/llm_chat_service.h"
#include "memory/daydream_consolidator.h"
#include "memory/memory_extractor.h"
#include "memory/memory_policy.h"
#include "memory/memory_retriever.h"
#include "memory/memory_store.h"
#include "memory/working_memory_cache.h"
#include "scheduler/daydream_trigger_policy.h"

class EmbeddingIndex; // 语义检索索引，可选注入；为空时 retrieve 走关键词路径
class AgentScheduler;  // Daydream 距待办判定用，可选注入
#include "router/intent_router.h"
#include "skill/skill_matcher.h"
#include "skill/skill_store.h"
#include "tool_registry.h"
#include "tools/runtime/tool_runtime.h"

class AIBrain : public QObject {
    Q_OBJECT

public:
    using EmotionSnapshotProvider = std::function<std::optional<EmotionSnapshot>()>;

    explicit AIBrain(QObject* parent = nullptr);

    void setPetName(const QString& petName);
    void setToolRegistry(ToolRegistry* registry);
    void setAgentScheduler(AgentScheduler* scheduler); // non-owning，供 Daydream 距待办判定
    void setEmotionSnapshotProvider(EmotionSnapshotProvider provider);

    // 注入通用提示词模版与独立身份基线（转发给 ContextBuilder）。
    void setIdentityBaseline(const IdentityBaseline& baseline) { m_contextBuilder.setIdentityBaseline(baseline); }
    void setPromptTemplate(const PromptTemplate& templ) { m_contextBuilder.setPromptTemplate(templ); }

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    void setThinkIntervalMs(int ms);

    void start();
    void stop();

    void triggerThink(const QString& reason = "manual",
                      const QString& triggerTag = "manual");
    void onUserInteraction(const QString& eventName, const QString& detail = QString());

    void clearMemory();
    void resolveToolConfirmation(const QString& requestId, bool approved);
    MemoryStore* memoryStore() { return &m_memoryStore; }
    const MemoryStore* memoryStore() const { return &m_memoryStore; }
    SkillStore* skillStore() { return &m_skillStore; }
    const SkillStore* skillStore() const { return &m_skillStore; }

    // 注入语义检索索引（绑定 SqliteEmbeddingIndex+OnnxEmbeddingProvider）。
    // non-owning；为 nullptr 时 retrieve 回退纯关键词打分。生命周期需长于本对象。
    void setEmbeddingIndex(EmbeddingIndex* idx) { m_embeddingIndex = idx; }
    EmbeddingIndex* embeddingIndex() const { return m_embeddingIndex; }

signals:
    void thinkingStarted(const QString& reason);
    void thinkingFinished(bool success, const QString& errorMessage);
    void assistantResponseReady(const QString& content);
    void proactiveResponseReady(const QString& content);
    void toolExecuted(const QString& toolName, bool success, const QString& payload);
    void toolConfirmationRequired(const QString& requestId,
                                  const QString& toolName,
                                  const QString& reason,
                                  const QJsonObject& arguments);
    void daydreamStarted(int itemCount);
    void daydreamFinished(const QJsonObject& summary);
    void daydreamCancelled(const QString& reason);

private:
    void thinkInternal(const QString& reason,
                      const QString& triggerTag,
                      int toolRound,
                      const QList<ChatMessage>& workingMessages);

    bool tryHandleRoutedIntent(const QString& reason,
                               const QString& triggerTag);
    bool shouldUseLocalRouter(const QString& triggerTag) const;
    ToolPolicyContext buildToolPolicyContext(const QString& triggerTag,
                                             const QString& userInput,
                                             bool initiatedByLlm) const;
    void rememberToolOutcome(const QString& toolName,
                             const QString& triggerTag,
                             bool initiatedByLlm,
                             const ToolExecutionOutcome& outcome);
    void processUserMemoryWrite(const QString& input,
                                const QString& triggerTag);
    std::optional<EmotionSnapshot> currentEmotionSnapshot() const;
    void annotateMemoryEntry(MemoryEntry& entry) const;
    QList<ChatMessage> buildBaseMessages(const QString& reason,
                                         const QString& triggerTag);
    QStringList retrieveMemoryHints(const QString& reason,
                                    const QString& triggerTag,
                                    int limit = 8);
    void appendToMemory(const ChatMessage& message);
    void setupTriggerTimers();
    void scheduleTrigger(const QString& triggerTag);
    // Daydream: snapshot batches are decided asynchronously, then committed once.
    void checkDaydreamTrigger();
    void runDaydreamSession();
    void runNextDaydreamBatch(quint64 generation);
    void finishDaydreamSession(quint64 generation);
    void cancelDaydreamSession(const QString& reason);
    bool canContinueDaydream() const;
    void armDaydreamTimer();
    AiTriggerConfig triggerConfigForTag(const QString& triggerTag) const;
    QStringList allowedActionsForTrigger(const QString& triggerTag) const;
    bool isToolCallAllowed(const QString& triggerTag,
                           const LlmToolCall& call,
                           QString& denialReason) const;
    void scheduleIdleRetryIfBusyFailure(const QString& toolName,
                                        const QString& toolPayload);

private:
    QString m_petName;
    ToolRegistry* m_toolRegistry = nullptr; // non-owning

    ContextBuilder m_contextBuilder;
    LlmChatService m_chatService;
    IntentRouter m_intentRouter;
    ToolRuntime m_toolRuntime;
    MemoryStore m_memoryStore;
    MemoryExtractor m_memoryExtractor;
    MemoryPolicy m_memoryPolicy;
    MemoryRetriever m_memoryRetriever;
    WorkingMemoryCache m_workingMemoryCache;
    SkillStore m_skillStore;
    SkillMatcher m_skillMatcher;
    EmbeddingIndex* m_embeddingIndex = nullptr; // non-owning，可选

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
    bool m_lastDaydreamInterrupted = false;
    quint64 m_daydreamGeneration = 0;
    DaydreamConsolidator::Snapshot m_daydreamSnapshot;
    QList<DaydreamConsolidator::Decision> m_daydreamDecisions;
    int m_daydreamBatchOffset = 0;
    int m_daydreamFallbackBatches = 0;
    int m_daydreamInvalidBatches = 0;

    bool m_enabled = true;
    bool m_running = false;
    bool m_busy = false;
    bool m_idleRetryScheduled = false;
    quint64 m_requestGeneration = 0;

    int m_maxToolRounds = 3;
    int m_maxMemoryMessages = 20;

    QList<ChatMessage> m_memory;
    QHash<QString, std::function<void(bool)>> m_pendingToolConfirmations;
    AiCallLogger m_callLogger;
    EmotionSnapshotProvider m_emotionSnapshotProvider;
};

#endif // DESKTOP_PET_AI_BRAIN_H
