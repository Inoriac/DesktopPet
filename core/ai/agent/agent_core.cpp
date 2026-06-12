#include "agent_core.h"

#include "ai/context/context_manager.h"
#include "ai/memory/memory_store.h"
#include "ai/tools/runtime/tool_runtime.h"
#include "ai/llm/llm_chat_service.h"
#include "configLoader/config_manager.h"

#include <QJsonDocument>
#include <QUuid>

AgentCore::AgentCore(QObject* parent)
    : QObject(parent) {}

void AgentCore::setPetName(const QString& petName) {
    m_petName = petName;
}

void AgentCore::setToolRuntime(ToolRuntime* runtime) {
    m_toolRuntime = runtime;
}

void AgentCore::setContextManager(ContextManager* contextManager) {
    m_contextManager = contextManager;
}

void AgentCore::setMemoryStore(MemoryStore* memoryStore) {
    m_memoryStore = memoryStore;
}

void AgentCore::setLlmChatService(LlmChatService* chatService) {
    m_chatService = chatService;
}

QString AgentCore::startTask(const QString& input, const QString& triggerTag) {
    AgentSession newSession = AgentSession::create(input, triggerTag);
    const QString sessionId = newSession.id();
    m_sessions.insert(sessionId, newSession);

    emit sessionStarted(sessionId, input);
    setSessionState(sessionId, AgentState::Understanding);

    if (!m_contextManager) {
        finishSession(sessionId, false, "ContextManager is not configured");
        return sessionId;
    }

    AgentContextRequest contextRequest;
    contextRequest.petName = m_petName;
    contextRequest.taskInput = input;
    contextRequest.reason = input;
    contextRequest.triggerTag = triggerTag;
    contextRequest.currentState = agentStateToString(AgentState::Understanding);
    contextRequest.availableTools = availableToolSchemas();

    if (m_memoryStore) {
        contextRequest.memoryHints = m_memoryStore->summaryForContext(8);
    }

    AgentSession* current = mutableSession(sessionId);
    if (current) {
        current->setMessages(m_contextManager->buildMessages(contextRequest));
    }

    setSessionState(sessionId, AgentState::Planning);

    if (!m_chatService || !m_toolRuntime) {
        finishSession(sessionId, false, "AgentCore skeleton created; planner/runtime is not connected yet");
        return sessionId;
    }

    continuePlanning(sessionId, 0);
    return sessionId;
}

bool AgentCore::hasSession(const QString& sessionId) const {
    return m_sessions.contains(sessionId);
}

const AgentSession* AgentCore::session(const QString& sessionId) const {
    auto it = m_sessions.constFind(sessionId);
    if (it == m_sessions.constEnd()) {
        return nullptr;
    }
    return &it.value();
}

AgentSession* AgentCore::mutableSession(const QString& sessionId) {
    auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end()) {
        return nullptr;
    }
    return &it.value();
}

void AgentCore::setSessionState(const QString& sessionId, AgentState state) {
    AgentSession* current = mutableSession(sessionId);
    if (!current) {
        return;
    }
    current->setState(state);
    emit sessionStateChanged(sessionId, current->stateName());
}

void AgentCore::finishSession(const QString& sessionId, bool success, const QString& message) {
    AgentSession* current = mutableSession(sessionId);
    if (!current) {
        return;
    }

    if (success) {
        current->setFinalResponse(message);
        setSessionState(sessionId, AgentState::Finished);
        if (!message.isEmpty()) {
            emit assistantResponseReady(sessionId, message);
        }
    } else {
        current->setErrorMessage(message);
        setSessionState(sessionId, AgentState::Failed);
    }

    emit sessionFinished(sessionId, success, message);
}

void AgentCore::continuePlanning(const QString& sessionId, int toolRound) {
    AgentSession* current = mutableSession(sessionId);
    if (!current || !m_chatService || !m_toolRuntime) {
        finishSession(sessionId, false, "AgentCore dependencies are not configured");
        return;
    }

    setSessionState(sessionId, AgentState::Planning);
    const QList<ChatMessage> messages = current->messages();
    const QJsonArray tools = availableToolSchemas();

    m_chatService->requestAsync(messages, tools,
        [this, sessionId, toolRound](bool ok, LlmResponse response, QString error) mutable {
            AgentSession* session = mutableSession(sessionId);
            if (!session) {
                return;
            }

            if (!ok) {
                finishSession(sessionId, false, error);
                return;
            }

            ChatMessage assistantMessage;
            assistantMessage.role = "assistant";
            assistantMessage.content = response.content;
            session->appendMessage(assistantMessage);
            const int assistantIndex = session->messages().size() - 1;

            if (response.toolCalls.isEmpty() || toolRound >= m_maxToolRounds) {
                if (m_memoryStore && !response.content.isEmpty()) {
                    m_memoryStore->add(MemoryType::ShortTerm,
                                       "assistant_response",
                                       response.content,
                                       {session->triggerTag(), "agent"});
                    m_memoryStore->save();
                }
                finishSession(sessionId, true, response.content);
                return;
            }

            QJsonArray assistantToolCalls;
            setSessionState(sessionId, AgentState::ExecutingTool);

            for (const LlmToolCall& call : response.toolCalls) {
                QJsonObject functionObj;
                functionObj["name"] = call.name;
                functionObj["arguments"] = QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact));

                QJsonObject toolCallObj;
                toolCallObj["id"] = call.id;
                toolCallObj["type"] = call.type.isEmpty() ? QString("function") : call.type;
                toolCallObj["function"] = functionObj;
                assistantToolCalls.append(toolCallObj);

                ToolExecutionRequest request;
                request.requestId = call.id.isEmpty()
                                        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                                        : call.id;
                request.toolName = call.name;
                request.arguments = call.arguments;
                request.policyContext.triggerTag = session->triggerTag();
                request.policyContext.userInput = session->input();
                request.policyContext.initiatedByLlm = true;
                request.policyContext.allowedRootPaths = ConfigManager::instance().getAiToolAccessPolicy().allowedRoots;
                request.policyContext.grantedToolNames = ConfigManager::instance().getAiToolAccessPolicy().autoGrantedTools;

                const ToolExecutionOutcome outcome = m_toolRuntime->execute(request);
                const QString payload = QString::fromUtf8(QJsonDocument(outcome.result.toJson()).toJson(QJsonDocument::Compact));

                if (outcome.policyDecision.needsConfirmation()) {
                    setSessionState(sessionId, AgentState::NeedUserConfirm);
                    emit userConfirmationRequired(sessionId, call.name, outcome.policyDecision.reason);
                }

                AgentToolObservation observation;
                observation.toolCallId = call.id;
                observation.toolName = call.name;
                observation.arguments = call.arguments;
                observation.result = outcome.result;
                observation.observedAt = QDateTime::currentDateTimeUtc();
                session->appendObservation(observation);

                ChatMessage toolMessage;
                toolMessage.role = "tool";
                toolMessage.name = call.name;
                toolMessage.toolCallId = call.id;
                toolMessage.content = payload;
                session->appendMessage(toolMessage);

                emit toolExecuted(sessionId, call.name, outcome.result.success, payload);
            }

            AgentSession* updated = mutableSession(sessionId);
            if (!updated) {
                return;
            }
            QList<ChatMessage>& mutableMessages = updated->mutableMessages();
            if (assistantIndex >= 0 && assistantIndex < mutableMessages.size()) {
                mutableMessages[assistantIndex].toolCalls = assistantToolCalls;
            }

            setSessionState(sessionId, AgentState::Observing);
            continuePlanning(sessionId, toolRound + 1);
        },
        m_petName);
}

QJsonArray AgentCore::availableToolSchemas() const {
    if (!m_toolRuntime || !m_toolRuntime->toolRegistry()) {
        return {};
    }
    return m_toolRuntime->toolRegistry()->allToolSchemas();
}