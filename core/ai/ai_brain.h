//
// AIBrain
// 负责 think 循环、上下文构建、LLM 请求和 tool 调度
//

#ifndef DESKTOP_PET_AI_BRAIN_H
#define DESKTOP_PET_AI_BRAIN_H

#include <QObject>
#include <QTimer>
#include <QList>
#include <QStringList>

#include "ai_types.h"
#include "ai_call_logger.h"
#include "context_builder.h"
#include "llm/llm_chat_service.h"
#include "memory/memory_extractor.h"
#include "memory/memory_policy.h"
#include "memory/memory_retriever.h"
#include "memory/memory_store.h"
#include "router/intent_router.h"
#include "tool_registry.h"
#include "tools/runtime/tool_runtime.h"

class AIBrain : public QObject {
    Q_OBJECT

public:
    explicit AIBrain(QObject* parent = nullptr);

    void setPetName(const QString& petName);
    void setToolRegistry(ToolRegistry* registry);

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    void setThinkIntervalMs(int ms);

    void start();
    void stop();

    void triggerThink(const QString& reason = "manual",
                      const QString& triggerTag = "manual");
    void onUserInteraction(const QString& eventName, const QString& detail = QString());

    void clearMemory();
    MemoryStore* memoryStore() { return &m_memoryStore; }
    const MemoryStore* memoryStore() const { return &m_memoryStore; }

signals:
    void thinkingStarted(const QString& reason);
    void thinkingFinished(bool success, const QString& errorMessage);
    void assistantResponseReady(const QString& content);
    void proactiveResponseReady(const QString& content);
    void toolExecuted(const QString& toolName, bool success, const QString& payload);

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
    void rememberAssistantResponse(const QString& content,
                                   const QString& triggerTag);
    void rememberToolOutcome(const QString& toolName,
                             const QString& triggerTag,
                             bool initiatedByLlm,
                             const ToolExecutionOutcome& outcome);
    void processUserMemoryWrite(const QString& input,
                                const QString& triggerTag);
    QList<ChatMessage> buildBaseMessages(const QString& reason,
                                         const QString& triggerTag) const;
    QStringList retrieveMemoryHints(const QString& reason,
                                    const QString& triggerTag,
                                    int limit = 8) const;
    void appendToMemory(const ChatMessage& message);
    void setupTriggerTimers();
    void scheduleTrigger(const QString& triggerTag);
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

    QTimer m_idleTriggerTimer;
    QTimer m_emotionTriggerTimer;
    QTimer m_chatTriggerTimer;

    bool m_enabled = true;
    bool m_running = false;
    bool m_busy = false;
    bool m_idleRetryScheduled = false;

    int m_maxToolRounds = 3;
    int m_maxMemoryMessages = 20;

    QList<ChatMessage> m_memory;
    AiCallLogger m_callLogger;
};

#endif // DESKTOP_PET_AI_BRAIN_H
