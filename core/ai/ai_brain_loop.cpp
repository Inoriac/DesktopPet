//
// AIBrain LLM loop and trigger scheduling
//

#include "ai_brain.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>

#include "configLoader/config_manager.h"
#include "memory/daydream_consolidator.h"
#include "scheduler/agent_scheduler.h"
#include "tools/environment_tools.h"

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
                const QString payload = m_toolRuntime.sanitizer()->toPayload(result);
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

    // Daydream 空闲整理监测 tick（4a：触发后同步跑 runHardcodedDrain 降级版）。
    m_daydreamTimer.setSingleShot(true);
    connect(&m_daydreamTimer, &QTimer::timeout, this, [this]() { checkDaydreamTrigger(); });
}

void AIBrain::armDaydreamTimer() {
    if (!m_running) return;
    m_daydreamTimer.start(m_daydreamPolicy.nextTickMs(0));
}

void AIBrain::checkDaydreamTrigger() {
    if (!m_running || m_daydreamRunning) {
        armDaydreamTimer();
        return;
    }

    const int idleSec = queryUserIdleSeconds();
    const qint64 msToNext = m_scheduler ? m_scheduler->msToNextDue() : -1;
    const QDateTime now = QDateTime::currentDateTime();

    // 小时窗口滚动：自首次/重置点起 1h 滚动计数（不按整点对齐，够用）。
    if (!m_daydreamHourAnchor.isValid() || now >= m_daydreamHourAnchor.addSecs(3600)) {
        m_daydreamCountThisHour = 0;
        m_daydreamHourAnchor = now;
    }
    const qint64 msSinceLast = m_lastDaydreamAt.isValid() ? m_lastDaydreamAt.msecsTo(now) : -1;

    if (m_daydreamPolicy.shouldTrigger(idleSec, m_busy, msToNext, msSinceLast,
                                       /*wasInterrupted=*/false, m_daydreamCountThisHour)) {
        runDaydreamSession();
    }
    armDaydreamTimer();
}

void AIBrain::runDaydreamSession() {
    m_daydreamRunning = true;
    m_lastDaydreamAt = QDateTime::currentDateTime();
    ++m_daydreamCountThisHour;

    DaydreamConsolidator consolidator(m_memoryStore);
    const DaydreamConsolidator::Stats stats = consolidator.runHardcodedDrain();
    qDebug() << "[Daydream] session done:"
             << "scanned=" << stats.scanned
             << "upgraded=" << stats.upgraded
             << "discarded=" << stats.discarded
             << "failed=" << stats.failed
             << "committed=" << stats.committed;

    m_daydreamRunning = false;
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
    if (call.name == "memory_organize") {
        if (triggerTag == "idle_action"
            || triggerTag == "proactive_chat"
            || triggerTag == "manual"
            || triggerTag == "user_request") {
            return true;
        }
        denialReason = QString("memory_organize is not allowed for trigger '%1'").arg(triggerTag);
        return false;
    }

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

