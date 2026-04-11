//
// AIBrain implementation
//

#include "ai_brain.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QUuid>

#include "configLoader/config_manager.h"

AIBrain::AIBrain(QObject* parent)
    : QObject(parent) {
    setupTriggerTimers();
}

void AIBrain::setPetName(const QString& petName) {
    m_petName = petName;
}

void AIBrain::setToolRegistry(ToolRegistry* registry) {
    m_toolRegistry = registry;
}

void AIBrain::setEnabled(bool enabled) {
    m_enabled = enabled;
    if (!m_enabled) {
        stop();
    }
}

void AIBrain::setThinkIntervalMs(int ms) {
    if (ms < 1000) {
        ms = 1000;
    }
    m_idleTriggerTimer.setInterval(ms);
}

void AIBrain::start() {
    if (!m_enabled || m_running) {
        return;
    }

    m_running = true;
    scheduleTrigger("idle_action");
    scheduleTrigger("emotion");
    scheduleTrigger("proactive_chat");
}

void AIBrain::stop() {
    m_running = false;
    m_idleTriggerTimer.stop();
    m_emotionTriggerTimer.stop();
    m_chatTriggerTimer.stop();
}

void AIBrain::triggerThink(const QString& reason,
                           const QString& triggerTag) {
    if (!m_enabled || m_busy) {
        return;
    }

    QList<ChatMessage> base = buildBaseMessages(reason, triggerTag);
    if (base.isEmpty()) {
        return;
    }

    emit thinkingStarted(reason);
    m_busy = true;
    thinkInternal(reason, triggerTag, 0, base);
}

void AIBrain::onUserInteraction(const QString& eventName, const QString& detail) {
    const QString reason = detail.isEmpty()
                             ? QString("user_event:%1").arg(eventName)
                             : QString("user_event:%1:%2").arg(eventName, detail);
    triggerThink(reason, "touch_event");
}

void AIBrain::clearMemory() {
    m_memory.clear();
}

QList<ChatMessage> AIBrain::buildBaseMessages(const QString& reason,
                                              const QString& triggerTag) const {
    QList<ChatMessage> messages;

    ChatMessage systemMessage;
    systemMessage.role = "system";
    systemMessage.content = m_contextBuilder.buildSystemPrompt(m_petName);
    messages.append(systemMessage);

    for (const ChatMessage& memoryMsg : m_memory) {
        messages.append(memoryMsg);
    }

    ChatMessage contextMessage;
    contextMessage.role = "user";
    contextMessage.content = m_contextBuilder.buildRuntimeContext(
        m_petName,
        reason,
        "Idle",
        triggerTag,
        allowedActionsForTrigger(triggerTag)
    );
    messages.append(contextMessage);

    return messages;
}

void AIBrain::appendToMemory(const ChatMessage& message) {
    m_memory.append(message);

    while (m_memory.size() > m_maxMemoryMessages) {
        m_memory.removeFirst();
    }
}

void AIBrain::thinkInternal(const QString& reason,
                            const QString& triggerTag,
                            int toolRound,
                            const QList<ChatMessage>& workingMessages) {
    const QJsonArray tools = m_toolRegistry ? m_toolRegistry->allToolSchemas() : QJsonArray{};
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    m_callLogger.logRequest(requestId,
                            m_petName,
                            reason,
                            triggerTag,
                            toolRound,
                            workingMessages,
                            tools);

    m_chatService.requestAsync(workingMessages, tools,
        [this, requestId, reason, triggerTag, toolRound, workingMessages](bool ok, LlmResponse response, QString error) mutable {
            m_callLogger.logResponse(requestId,
                                     m_petName,
                                     ok,
                                     response,
                                     error);

            if (!ok) {
                m_busy = false;
                emit thinkingFinished(false, error);
                if (m_running) {
                    scheduleTrigger(triggerTag);
                }
                return;
            }

            ChatMessage assistantMessage;
            assistantMessage.role = "assistant";
            assistantMessage.content = response.content;

            if (response.toolCalls.isEmpty() || !m_toolRegistry || toolRound >= m_maxToolRounds) {
                appendToMemory(assistantMessage);
                if (!response.content.isEmpty()) {
                    emit assistantResponseReady(response.content);
                }

                m_busy = false;
                emit thinkingFinished(true, {});
                if (m_running) {
                    scheduleTrigger(triggerTag);
                }
                return;
            }

            QJsonArray assistantToolCalls;
            QList<ChatMessage> nextMessages = workingMessages;
            nextMessages.append(assistantMessage);

            for (const LlmToolCall& call : response.toolCalls) {
                QString denialReason;
                if (!isToolCallAllowed(triggerTag, call, denialReason)) {
                    QJsonObject deniedObj;
                    deniedObj["success"] = false;
                    deniedObj["error"] = denialReason;

                    ChatMessage deniedToolMessage;
                    deniedToolMessage.role = "tool";
                    deniedToolMessage.name = call.name;
                    deniedToolMessage.toolCallId = call.id;
                    deniedToolMessage.content = QString::fromUtf8(QJsonDocument(deniedObj).toJson(QJsonDocument::Compact));
                    nextMessages.append(deniedToolMessage);
                    appendToMemory(deniedToolMessage);

                    emit toolExecuted(call.name, false, deniedToolMessage.content);
                    continue;
                }

                QJsonObject functionObj;
                functionObj["name"] = call.name;
                functionObj["arguments"] = QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact));

                QJsonObject toolCallObj;
                toolCallObj["id"] = call.id;
                toolCallObj["type"] = call.type.isEmpty() ? QString("function") : call.type;
                toolCallObj["function"] = functionObj;
                assistantToolCalls.append(toolCallObj);

                ToolResult result = m_toolRegistry->executeTool(call.name, call.arguments);
                const QString payload = QString::fromUtf8(QJsonDocument(result.toJson()).toJson(QJsonDocument::Compact));

                ChatMessage toolMessage;
                toolMessage.role = "tool";
                toolMessage.name = call.name;
                toolMessage.content = payload;
                toolMessage.toolCallId = call.id;

                nextMessages.append(toolMessage);
                appendToMemory(toolMessage);

                emit toolExecuted(call.name, result.success, payload);
            }

            nextMessages[nextMessages.size() - response.toolCalls.size() - 1].toolCalls = assistantToolCalls;
            thinkInternal(reason, triggerTag, toolRound + 1, nextMessages);
        },
        m_petName);
}

void AIBrain::setupTriggerTimers() {
    m_idleTriggerTimer.setSingleShot(true);
    m_emotionTriggerTimer.setSingleShot(true);
    m_chatTriggerTimer.setSingleShot(true);

    connect(&m_idleTriggerTimer, &QTimer::timeout, this, [this]() {
        if (m_busy) {
            scheduleTrigger("idle_action");
            return;
        }
        triggerThink("idle_tick", "idle_action");
    });
    connect(&m_emotionTriggerTimer, &QTimer::timeout, this, [this]() {
        if (m_busy) {
            scheduleTrigger("emotion");
            return;
        }
        triggerThink("emotion_tick", "emotion");
    });
    connect(&m_chatTriggerTimer, &QTimer::timeout, this, [this]() {
        if (m_busy) {
            scheduleTrigger("proactive_chat");
            return;
        }
        triggerThink("proactive_chat_tick", "proactive_chat");
    });
}

AiTriggerConfig AIBrain::triggerConfigForTag(const QString& triggerTag) const {
    const AiBehaviorPolicy& policy = ConfigManager::instance().getAiBehaviorPolicy();
    if (triggerTag == "idle_action") return policy.idleTrigger;
    if (triggerTag == "emotion") return policy.emotionTrigger;
    if (triggerTag == "proactive_chat") return policy.proactiveChatTrigger;

    AiTriggerConfig fallback;
    fallback.enabled = true;
    fallback.minIntervalMs = 60000;
    fallback.maxIntervalMs = 120000;
    return fallback;
}

void AIBrain::scheduleTrigger(const QString& triggerTag) {
    if (!m_running) {
        return;
    }

    const AiTriggerConfig cfg = triggerConfigForTag(triggerTag);
    if (!cfg.enabled) {
        return;
    }

    const int interval = QRandomGenerator::global()->bounded(cfg.minIntervalMs, cfg.maxIntervalMs + 1);

    if (triggerTag == "idle_action") {
        m_idleTriggerTimer.start(interval);
    } else if (triggerTag == "emotion") {
        m_emotionTriggerTimer.start(interval);
    } else if (triggerTag == "proactive_chat") {
        m_chatTriggerTimer.start(interval);
    }
}

QStringList AIBrain::allowedActionsForTrigger(const QString& triggerTag) const {
    const AiBehaviorPolicy& policy = ConfigManager::instance().getAiBehaviorPolicy();
    if (triggerTag == "idle_action") return policy.idleActionWhitelist;
    if (triggerTag == "touch_event") return policy.touchActionWhitelist;
    if (triggerTag == "emotion") return policy.emotionActionWhitelist;
    return QStringList{};
}

bool AIBrain::isToolCallAllowed(const QString& triggerTag,
                                const LlmToolCall& call,
                                QString& denialReason) const {
    // 仅约束主动动作切换类 tool，其它 tool 默认允许。
    if (call.name != "play_animation") {
        return true;
    }

    const QString state = call.arguments.value("state").toString();
    if (state.isEmpty()) {
        denialReason = "play_animation missing required field: state";
        return false;
    }

    const AiBehaviorPolicy& policy = ConfigManager::instance().getAiBehaviorPolicy();
    if (policy.forbiddenActions.contains(state, Qt::CaseInsensitive)) {
        denialReason = QString("Action '%1' is forbidden by policy").arg(state);
        return false;
    }

    const QStringList allowed = allowedActionsForTrigger(triggerTag);
    if (!allowed.isEmpty() && !allowed.contains(state, Qt::CaseInsensitive)) {
        denialReason = QString("Action '%1' is not in whitelist for trigger '%2'").arg(state, triggerTag);
        return false;
    }

    return true;
}
