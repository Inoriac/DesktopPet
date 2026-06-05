#include "agent_session.h"

#include <QJsonArray>
#include <QUuid>

#include <utility>

namespace {
QJsonObject chatMessageToJson(const ChatMessage& message) {
    QJsonObject obj;
    obj["role"] = message.role;
    obj["content"] = message.content;
    if (!message.name.isEmpty()) obj["name"] = message.name;
    if (!message.toolCallId.isEmpty()) obj["tool_call_id"] = message.toolCallId;
    if (!message.toolCalls.isEmpty()) obj["tool_calls"] = message.toolCalls;
    return obj;
}
}

QJsonObject AgentToolObservation::toJson() const {
    QJsonObject obj;
    obj["tool_call_id"] = toolCallId;
    obj["tool_name"] = toolName;
    obj["arguments"] = arguments;
    obj["result"] = result.toJson();
    obj["observed_at"] = observedAt.toString(Qt::ISODate);
    return obj;
}

AgentSession::AgentSession()
    : AgentSession(QString(), QString(), QString()) {}

AgentSession::AgentSession(QString id, QString input, QString triggerTag)
    : m_id(std::move(id))
    , m_input(std::move(input))
    , m_triggerTag(std::move(triggerTag))
    , m_createdAt(QDateTime::currentDateTimeUtc())
    , m_updatedAt(m_createdAt) {}

AgentSession AgentSession::create(const QString& input, const QString& triggerTag) {
    return AgentSession(QUuid::createUuid().toString(QUuid::WithoutBraces), input, triggerTag);
}

void AgentSession::setState(AgentState state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    touch();
}

void AgentSession::setMessages(const QList<ChatMessage>& messages) {
    m_messages = messages;
    touch();
}

void AgentSession::appendMessage(const ChatMessage& message) {
    m_messages.append(message);
    touch();
}

void AgentSession::appendObservation(const AgentToolObservation& observation) {
    m_observations.append(observation);
    touch();
}

void AgentSession::setFinalResponse(const QString& response) {
    m_finalResponse = response;
    touch();
}

void AgentSession::setErrorMessage(const QString& errorMessage) {
    m_errorMessage = errorMessage;
    touch();
}

QJsonObject AgentSession::toJson() const {
    QJsonArray messages;
    for (const ChatMessage& message : m_messages) {
        messages.append(chatMessageToJson(message));
    }

    QJsonArray observations;
    for (const AgentToolObservation& observation : m_observations) {
        observations.append(observation.toJson());
    }

    QJsonObject obj;
    obj["id"] = m_id;
    obj["input"] = m_input;
    obj["trigger_tag"] = m_triggerTag;
    obj["state"] = stateName();
    obj["created_at"] = m_createdAt.toString(Qt::ISODate);
    obj["updated_at"] = m_updatedAt.toString(Qt::ISODate);
    obj["messages"] = messages;
    obj["observations"] = observations;
    if (!m_finalResponse.isEmpty()) obj["final_response"] = m_finalResponse;
    if (!m_errorMessage.isEmpty()) obj["error_message"] = m_errorMessage;
    return obj;
}

void AgentSession::touch() {
    m_updatedAt = QDateTime::currentDateTimeUtc();
}