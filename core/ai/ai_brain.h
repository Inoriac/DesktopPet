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
#include "tool_registry.h"

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

signals:
    void thinkingStarted(const QString& reason);
    void thinkingFinished(bool success, const QString& errorMessage);
    void assistantResponseReady(const QString& content);
    void toolExecuted(const QString& toolName, bool success, const QString& payload);

private:
    void thinkInternal(const QString& reason,
                      const QString& triggerTag,
                      int toolRound,
                      const QList<ChatMessage>& workingMessages);

    QList<ChatMessage> buildBaseMessages(const QString& reason,
                                         const QString& triggerTag) const;
    void appendToMemory(const ChatMessage& message);
    void setupTriggerTimers();
    void scheduleTrigger(const QString& triggerTag);
    AiTriggerConfig triggerConfigForTag(const QString& triggerTag) const;
    QStringList allowedActionsForTrigger(const QString& triggerTag) const;
    bool isToolCallAllowed(const QString& triggerTag,
                           const LlmToolCall& call,
                           QString& denialReason) const;

private:
    QString m_petName;
    ToolRegistry* m_toolRegistry = nullptr; // non-owning

    ContextBuilder m_contextBuilder;
    LlmChatService m_chatService;

    QTimer m_idleTriggerTimer;
    QTimer m_emotionTriggerTimer;
    QTimer m_chatTriggerTimer;

    bool m_enabled = true;
    bool m_running = false;
    bool m_busy = false;

    int m_maxToolRounds = 3;
    int m_maxMemoryMessages = 20;

    QList<ChatMessage> m_memory;
    AiCallLogger m_callLogger;
};

#endif // DESKTOP_PET_AI_BRAIN_H
