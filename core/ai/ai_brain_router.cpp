//
// AIBrain routing and memory helpers
//

#include "ai_brain.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include "tools/runtime/tool_policy.h"

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
            } else if (route.toolName == "weather_query") {
                const QJsonObject data = outcome.result.data;
                const QString location = data.value("location").toString().trimmed();
                const QString description = data.value("description").toString().trimmed();
                const QString temperature = data.value("temperature_c").toString().trimmed();
                const QString feelsLike = data.value("feels_like_c").toString().trimmed();
                const QString humidity = data.value("humidity").toString().trimmed();
                const QString wind = data.value("wind_kmph").toString().trimmed();

                QStringList details;
                if (!description.isEmpty()) {
                    details.append(description);
                }
                if (!temperature.isEmpty()) {
                    details.append(QString("气温 %1°C").arg(temperature));
                }
                if (!feelsLike.isEmpty()) {
                    details.append(QString("体感 %1°C").arg(feelsLike));
                }
                if (!humidity.isEmpty()) {
                    details.append(QString("湿度 %1%").arg(humidity));
                }
                if (!wind.isEmpty()) {
                    details.append(QString("风速 %1 km/h").arg(wind));
                }

                responseText = details.isEmpty()
                    ? QString("天气查询成功，但没有获取到详细信息。")
                    : QString("%1当前天气：%2。")
                        .arg(location.isEmpty() ? QString() : location + QStringLiteral(" "),
                             details.join("，"));
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

