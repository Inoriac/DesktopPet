#ifndef DESKTOP_PET_AGENT_CORE_H
#define DESKTOP_PET_AGENT_CORE_H

#include <QHash>
#include <QObject>
#include <QJsonArray>
#include <QString>

#include "agent_session.h"

class ContextManager;
class LlmChatService;
class MemoryStore;
class ToolRuntime;

class AgentCore : public QObject {
    Q_OBJECT

public:
    explicit AgentCore(QObject* parent = nullptr);

    void setPetName(const QString& petName);
    void setToolRuntime(ToolRuntime* runtime);
    void setContextManager(ContextManager* contextManager);
    void setMemoryStore(MemoryStore* memoryStore);
    void setLlmChatService(LlmChatService* chatService);

    QString startTask(const QString& input, const QString& triggerTag = "user_request");
    bool hasSession(const QString& sessionId) const;
    const AgentSession* session(const QString& sessionId) const;

signals:
    void sessionStarted(const QString& sessionId, const QString& input);
    void sessionStateChanged(const QString& sessionId, const QString& state);
    void assistantResponseReady(const QString& sessionId, const QString& content);
    void toolExecuted(const QString& sessionId, const QString& toolName, bool success, const QString& payload);
    void userConfirmationRequired(const QString& sessionId, const QString& toolName, const QString& reason);
    void sessionFinished(const QString& sessionId, bool success, const QString& message);

private:
    AgentSession* mutableSession(const QString& sessionId);
    void setSessionState(const QString& sessionId, AgentState state);
    void finishSession(const QString& sessionId, bool success, const QString& message);
    void continuePlanning(const QString& sessionId, int toolRound);
    QJsonArray availableToolSchemas() const;

private:
    QString m_petName;
    ToolRuntime* m_toolRuntime = nullptr;       // non-owning
    ContextManager* m_contextManager = nullptr; // non-owning
    MemoryStore* m_memoryStore = nullptr;       // non-owning
    LlmChatService* m_chatService = nullptr;    // non-owning
    QHash<QString, AgentSession> m_sessions;
    int m_maxToolRounds = 3;
};

#endif // DESKTOP_PET_AGENT_CORE_H