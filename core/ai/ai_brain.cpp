//
// AIBrain implementation
//

#include "ai_brain.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QCoreApplication>
#include <QDir>
#include <QUuid>

#include "configLoader/config_manager.h"

AIBrain::AIBrain(QObject* parent)
    : QObject(parent) {
    setupTriggerTimers();
    m_memoryStore.load();
}

void AIBrain::setPetName(const QString& petName) {
    m_petName = petName;
}

void AIBrain::setToolRegistry(ToolRegistry* registry) {
    m_toolRegistry = registry;
    m_toolRuntime.setToolRegistry(registry);
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
    m_idleRetryScheduled = false;
}

void AIBrain::triggerThink(const QString& reason,
                           const QString& triggerTag) {
    if (!m_enabled || m_busy) {
        return;
    }

    if (shouldUseLocalRouter(triggerTag) && tryHandleRoutedIntent(reason, triggerTag)) {
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
    m_memoryStore.clear();
    m_memoryStore.save();
}

bool AIBrain::tryHandleRoutedIntent(const QString& reason,
                                    const QString& triggerTag) {
    const IntentRoute route = m_intentRouter.route(reason, triggerTag);
    if (route.type == IntentRouteType::NeedLLM) {
        return false;
    }

    emit thinkingStarted(reason);
    m_busy = true;

    if (route.type == IntentRouteType::DirectReply
        || route.type == IntentRouteType::NeedClarification
        || route.type == IntentRouteType::Rejected) {
        if (!route.reply.isEmpty()) {
            emit assistantResponseReady(route.reply);

            ChatMessage assistantMessage;
            assistantMessage.role = "assistant";
            assistantMessage.content = route.reply;
            appendToMemory(assistantMessage);
            rememberAssistantResponse(route.reply, triggerTag);
        }

        m_busy = false;
        emit thinkingFinished(route.type != IntentRouteType::Rejected, route.type == IntentRouteType::Rejected ? route.reason : QString());
        return true;
    }

    if (route.type == IntentRouteType::DirectToolCall) {
        ToolExecutionRequest request;
        request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        request.toolName = route.toolName;
        request.arguments = route.toolArguments;
        request.policyContext = buildToolPolicyContext(triggerTag, reason, false);
        request.userConfirmed = false;

        const ToolExecutionOutcome outcome = m_toolRuntime.execute(request);
        const QString payload = QString::fromUtf8(QJsonDocument(outcome.result.toJson()).toJson(QJsonDocument::Compact));
    rememberToolOutcome(route.toolName, triggerTag, false, outcome);

        emit toolExecuted(route.toolName, outcome.result.success, payload);

        QString responseText;
        if (outcome.policyDecision.needsConfirmation()) {
            responseText = QString("这个操作需要你确认后才能执行：%1").arg(outcome.policyDecision.reason);
        } else if (outcome.result.success) {
            if (route.toolName == "lx_music_status") {
                const QString status = outcome.result.data.value("status").toString();
                const QString name = outcome.result.data.value("name").toString();
                const QString singer = outcome.result.data.value("singer").toString();
                responseText = name.isEmpty()
                    ? QString("LX Music 当前状态：%1。") .arg(status.isEmpty() ? QString("未知") : status)
                    : QString("LX Music 当前%1：%2 - %3。")
                        .arg(status == "playing" ? QString("正在播放") : QString("状态为%1").arg(status), name, singer);
            } else if (route.toolName == "lx_music_lyric") {
                responseText = outcome.result.data.value("text").toString().trimmed();
                if (responseText.size() > 160) {
                    responseText = responseText.left(160) + "...";
                }
                if (responseText.isEmpty()) {
                    responseText = "当前没有获取到歌词。";
                }
            } else if (route.toolName == "lx_music_list_playlists") {
                const QJsonArray items = outcome.result.data.value("items").toArray();
                QStringList names;
                for (int i = 0; i < items.size() && i < 5; ++i) {
                    names.append(items.at(i).toObject().value("name").toString());
                }
                responseText = QString("找到 %1 个 LX Music 歌单：%2。")
                    .arg(items.size())
                    .arg(names.join("、"));
            } else {
                responseText = outcome.result.data.value("time").toString();
            }

            if (responseText.isEmpty()) {
                responseText = "已完成。";
            } else if (route.toolName == "get_current_time") {
                responseText = QString("现在是 %1。").arg(responseText);
            }
        } else {
            responseText = QString("执行失败：%1").arg(outcome.result.errorMessage);
        }

        emit assistantResponseReady(responseText);

        ChatMessage toolMessage;
        toolMessage.role = "tool";
        toolMessage.name = route.toolName;
        toolMessage.content = payload;
        appendToMemory(toolMessage);

        ChatMessage assistantMessage;
        assistantMessage.role = "assistant";
        assistantMessage.content = responseText;
        appendToMemory(assistantMessage);
        rememberAssistantResponse(responseText, triggerTag);

        m_busy = false;
        emit thinkingFinished(outcome.result.success, outcome.result.success ? QString() : outcome.result.errorMessage);
        return true;
    }

    m_busy = false;
    emit thinkingFinished(false, "unsupported route type");
    return true;
}

bool AIBrain::shouldUseLocalRouter(const QString& triggerTag) const {
    return triggerTag == "manual" || triggerTag == "user_request";
}

ToolPolicyContext AIBrain::buildToolPolicyContext(const QString& triggerTag,
                                                  const QString& userInput,
                                                  bool initiatedByLlm) const {
    ToolPolicyContext context;
    context.triggerTag = triggerTag;
    context.userInput = userInput;
    context.initiatedByLlm = initiatedByLlm;
    context.allowedRootPaths.append(QCoreApplication::applicationDirPath());
    context.allowedRootPaths.append(QDir::currentPath());
    return context;
}

void AIBrain::rememberAssistantResponse(const QString& content,
                                        const QString& triggerTag) {
    if (content.isEmpty()) {
        return;
    }

    m_memoryStore.add(MemoryType::ShortTerm,
                      "assistant_response",
                      content,
                      {triggerTag, "assistant"});
    m_memoryStore.save();
}

void AIBrain::rememberToolOutcome(const QString& toolName,
                                  const QString& triggerTag,
                                  bool initiatedByLlm,
                                  const ToolExecutionOutcome& outcome) {
    QJsonObject event;
    event["tool_name"] = toolName;
    event["request_id"] = outcome.requestId;
    event["executed"] = outcome.executed;
    event["success"] = outcome.result.success;
    event["policy_action"] = toolPolicyActionToString(outcome.policyDecision.action);
    event["risk_level"] = toolRiskLevelToString(outcome.policyDecision.riskLevel);
    event["policy_reason"] = outcome.policyDecision.reason;
    if (!outcome.result.success) {
        event["error"] = outcome.result.errorMessage;
    }

    m_memoryStore.add(MemoryType::Event,
                      "tool_execution",
                      event,
                      {triggerTag, initiatedByLlm ? "llm" : "router", toolName});
    m_memoryStore.save();
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
                    if (triggerTag == "proactive_chat") {
                        emit proactiveResponseReady(response.content);
                    }
                    rememberAssistantResponse(response.content, triggerTag);
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
            const int assistantIndex = nextMessages.size() - 1;

            for (const LlmToolCall& call : response.toolCalls) {
                QJsonObject functionObj;
                functionObj["name"] = call.name;
                functionObj["arguments"] = QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact));

                QJsonObject toolCallObj;
                toolCallObj["id"] = call.id;
                toolCallObj["type"] = call.type.isEmpty() ? QString("function") : call.type;
                toolCallObj["function"] = functionObj;
                assistantToolCalls.append(toolCallObj);

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

                ToolExecutionRequest executionRequest;
                executionRequest.requestId = call.id.isEmpty()
                                             ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                                             : call.id;
                executionRequest.toolName = call.name;
                executionRequest.arguments = call.arguments;
                executionRequest.policyContext = buildToolPolicyContext(triggerTag, reason, true);
                executionRequest.userConfirmed = false;

                const ToolExecutionOutcome outcome = m_toolRuntime.execute(executionRequest);
                ToolResult result = outcome.result;
                const QString payload = QString::fromUtf8(QJsonDocument(result.toJson()).toJson(QJsonDocument::Compact));
                rememberToolOutcome(call.name, triggerTag, true, outcome);

                if (!result.success) {
                    scheduleIdleRetryIfBusyFailure(call.name, payload);
                }

                ChatMessage toolMessage;
                toolMessage.role = "tool";
                toolMessage.name = call.name;
                toolMessage.content = payload;
                toolMessage.toolCallId = call.id;

                nextMessages.append(toolMessage);
                appendToMemory(toolMessage);

                emit toolExecuted(call.name, result.success, payload);
            }

            if (assistantIndex >= 0 && assistantIndex < nextMessages.size()) {
                nextMessages[assistantIndex].toolCalls = assistantToolCalls;
            }
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
    if (call.name != "play_animation" && call.name != "request_idle_transition") {
        return true;
    }

    QString state;
    if (call.name == "request_idle_transition") {
        state = call.arguments.value("target_action").toString();
    } else {
        state = call.arguments.value("state").toString();
    }
    if (state.isEmpty()) {
        denialReason = QString("%1 missing required state field").arg(call.name);
        return false;
    }

    if (state.startsWith("Touch", Qt::CaseInsensitive)) {
        denialReason = QString("Action '%1' is touch-only and managed by local interaction pipeline").arg(state);
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

void AIBrain::scheduleIdleRetryIfBusyFailure(const QString& toolName,
                                             const QString& toolPayload) {
    if (toolName != "play_animation" && toolName != "request_idle_transition") {
        return;
    }
    if (m_idleRetryScheduled) {
        return;
    }

    const QJsonDocument payloadDoc = QJsonDocument::fromJson(toolPayload.toUtf8());
    if (!payloadDoc.isObject()) {
        return;
    }
    const QJsonObject payloadObj = payloadDoc.object();
    if (payloadObj.value("success").toBool(true)) {
        return;
    }

    const QString errorMessage = payloadObj.value("error").toString();
    if (!errorMessage.contains("busy", Qt::CaseInsensitive)) {
        return;
    }

    m_idleRetryScheduled = true;
    const int delayMs = QRandomGenerator::global()->bounded(3000, 8001);
    QTimer::singleShot(delayMs, this, [this]() {
        m_idleRetryScheduled = false;

        if (!m_running || !m_enabled) {
            return;
        }

        if (m_busy) {
            scheduleTrigger("idle_action");
            return;
        }

        triggerThink("busy_retry", "idle_action");
    });
}
