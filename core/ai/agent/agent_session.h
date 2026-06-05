#ifndef DESKTOP_PET_AGENT_SESSION_H
#define DESKTOP_PET_AGENT_SESSION_H

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

#include "ai_types.h"
#include "agent_state.h"

struct AgentToolObservation {
    QString toolCallId;
    QString toolName;
    QJsonObject arguments;
    ToolResult result;
    QDateTime observedAt;

    QJsonObject toJson() const;
};

class AgentSession {
public:
    AgentSession();
    AgentSession(QString id, QString input, QString triggerTag);

    static AgentSession create(const QString& input, const QString& triggerTag);

    const QString& id() const { return m_id; }
    const QString& input() const { return m_input; }
    const QString& triggerTag() const { return m_triggerTag; }

    AgentState state() const { return m_state; }
    QString stateName() const { return agentStateToString(m_state); }
    void setState(AgentState state);

    const QDateTime& createdAt() const { return m_createdAt; }
    const QDateTime& updatedAt() const { return m_updatedAt; }

    const QList<ChatMessage>& messages() const { return m_messages; }
    QList<ChatMessage>& mutableMessages() { return m_messages; }
    void setMessages(const QList<ChatMessage>& messages);
    void appendMessage(const ChatMessage& message);

    const QList<AgentToolObservation>& observations() const { return m_observations; }
    void appendObservation(const AgentToolObservation& observation);

    const QString& finalResponse() const { return m_finalResponse; }
    void setFinalResponse(const QString& response);

    const QString& errorMessage() const { return m_errorMessage; }
    void setErrorMessage(const QString& errorMessage);

    QJsonObject toJson() const;

private:
    void touch();

private:
    QString m_id;
    QString m_input;
    QString m_triggerTag;
    AgentState m_state = AgentState::Idle;
    QDateTime m_createdAt;
    QDateTime m_updatedAt;
    QList<ChatMessage> m_messages;
    QList<AgentToolObservation> m_observations;
    QString m_finalResponse;
    QString m_errorMessage;
};

#endif // DESKTOP_PET_AGENT_SESSION_H