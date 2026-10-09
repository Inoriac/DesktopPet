//
// AIBrain routing and memory helpers
//

#include "ai_brain.h"
#include "memory/memory_metadata.h"

#include "chat/chat_preparation_types.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QSet>
#include <algorithm>

#include "configLoader/config_manager.h"
#include "runtime/agent_runtime_services.h"
#include "tools/runtime/tool_policy.h"
#include "memory/working_memory_cache.h"

namespace {
QString reminderTime(const QJsonObject& task) {
    const auto time = QDateTime::fromString(task.value("next_trigger_at").toString(), Qt::ISODate);
    return time.isValid() ? time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                          : QStringLiteral("未安排");
}

QString reminderDescription(const QJsonObject& task) {
    const QString title = task.value("title").toString();
    const QString message = task.value("message").toString();
    return message.isEmpty() || message == title ? title
        : QStringLiteral("%1：%2").arg(title, message);
}

QString scheduleResponse(const QString& toolName, const QJsonObject& data) {
    if (toolName == QLatin1String("schedule_create")) {
        const auto task = data.value("task").toObject();
        return QStringLiteral("已创建提醒：%1\nID：%2\n下次提醒：%3（本地时间）")
            .arg(reminderDescription(task), task.value("id").toString(), reminderTime(task));
    }
    if (toolName == QLatin1String("schedule_cancel")) {
        return QStringLiteral("已取消提醒，ID：%1。").arg(data.value("id").toString());
    }
    if (toolName == QLatin1String("schedule_snooze")) {
        const auto task = data.value("task").toObject();
        return QStringLiteral("已推迟提醒：%1\nID：%2\n下次提醒：%3（本地时间）")
            .arg(reminderDescription(task), data.value("id").toString(),
                 reminderTime(task.isEmpty() ? data : task));
    }
    if (toolName != QLatin1String("schedule_list")) return {};

    const auto tasks = data.value("tasks").toArray();
    const int total = data.value("total_count").toInt(tasks.size());
    QStringList lines;
    lines.append(total == 0 ? QStringLiteral("当前没有待提醒任务。")
                           : QStringLiteral("当前有 %1 个提醒，本页显示 %2 个：").arg(total).arg(tasks.size()));
    for (const auto& value : tasks) {
        const auto task = value.toObject();
        lines.append(QStringLiteral("%1%2\nID：%3；下次提醒：%4（本地时间）")
            .arg(task.value("enabled").toBool(true) ? QString() : QStringLiteral("[已停用] "),
                 reminderDescription(task), task.value("id").toString(), reminderTime(task)));
    }
    if (data.value("has_more").toBool())
        lines.append(QStringLiteral("还有提醒未显示，可以继续查询更多提醒。"));
    const auto completed = data.value("recent_completed").toArray();
    const int completedTotal = data.value("recent_completed_total").toInt(completed.size());
    if (completedTotal > 0) {
        lines.append(QStringLiteral("最近已完成的提醒（共 %1 个，本页 %2 个；可按 ID 稍后再提醒）：")
            .arg(completedTotal).arg(completed.size()));
        for (const auto& value : completed) {
            const auto task = value.toObject();
            lines.append(QStringLiteral("%1\nID：%2").arg(
                reminderDescription(task), task.value("id").toString()));
        }
        if (data.value("completed_has_more").toBool())
            lines.append(QStringLiteral("还有已完成记录未显示，可以继续查询。"));
    }
    return lines.join(QLatin1Char('\n'));
}
}

bool AIBrain::tryHandleRoutedIntent(const IntentRoute& route,
                                    const QString& reason,
                                    const QString& triggerTag,
                                    const QString& sessionId) {
    if (route.type == IntentRouteType::NeedLLM) {
        return false;
    }

    if (route.type == IntentRouteType::DirectReply
        || route.type == IntentRouteType::NeedClarification
        || route.type == IntentRouteType::Rejected) {
        appendActiveDelta(route.reply);
        const bool rejected = route.type == IntentRouteType::Rejected;
        finishActiveResponse(rejected ? ChatMessageStatus::Failed
                                      : ChatMessageStatus::Complete,
                             rejected ? route.reason : QString());
        return true;
    }

    if (route.type == IntentRouteType::DirectToolCall) {
        publishActiveStage(ChatActivityStage::PreparingTool);
        publishActiveStage(ChatActivityStage::RunningTool);
        ToolExecutionRequest request;
        request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        request.toolName = route.toolName;
        request.arguments = route.toolArguments;
        request.policyContext = buildToolPolicyContext(triggerTag, reason, false);
        const ToolExecutionOutcome outcome = m_toolRuntime.execute(request);
        if (outcome.policyDecision.needsConfirmation()) {
            const QString confirmationId = outcome.requestId;
            const QString toolName = route.toolName;
            m_pendingToolConfirmations.insert(
                confirmationId,
                [this, confirmationId, toolName, triggerTag, sessionId](bool approved) {
                    const ToolExecutionOutcome resolved =
                        m_toolRuntime.resolveConfirmation(confirmationId, approved);
                    const QString resolvedPayload =
                        m_toolRuntime.sanitizer()->toPayload(resolved.result);
                    rememberToolOutcome(toolName, triggerTag, false, resolved, sessionId);
                    emit toolExecuted(toolName, resolved.result.success, resolvedPayload);

                    ChatMessage toolMessage;
                    toolMessage.role = "tool";
                    toolMessage.name = toolName;
                    toolMessage.content = resolvedPayload;

                    QString responseText = resolved.result.success
                        ? scheduleResponse(toolName, resolved.result.data)
                        : QStringLiteral("操作未执行：%1").arg(resolved.result.errorMessage);
                    if (responseText.isEmpty()) responseText = QStringLiteral("已完成。");
                    publishActiveStage(ChatActivityStage::Finalizing);
                    appendActiveDelta(responseText);
                    publishActiveStage(ChatActivityStage::Finalizing);
                    finishActiveResponse(resolved.result.success
                                             ? ChatMessageStatus::Complete
                                             : ChatMessageStatus::Failed,
                                         resolved.result.success
                                             ? QString()
                                             : resolved.result.errorMessage);
                });
            emit toolConfirmationRequired(confirmationId,
                                          route.toolName,
                                          outcome.policyDecision.reason,
                                          route.toolArguments);
            return true;
        }
        const QString payload = m_toolRuntime.sanitizer()->toPayload(outcome.result);
        rememberToolOutcome(route.toolName, triggerTag, false, outcome, sessionId);

        emit toolExecuted(route.toolName, outcome.result.success, payload);

        QString responseText;
        if (outcome.policyDecision.needsConfirmation()) {
            responseText = QString("这个操作需要你确认后才能执行：%1").arg(outcome.policyDecision.reason);
        } else if (outcome.result.success) {
            if (route.toolName.startsWith(QLatin1String("schedule_"))) {
                responseText = scheduleResponse(route.toolName, outcome.result.data);
            } else if (route.toolName == "lx_music_status") {
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

        ChatMessage toolMessage;
        toolMessage.role = "tool";
        toolMessage.name = route.toolName;
        toolMessage.content = payload;

        publishActiveStage(ChatActivityStage::Finalizing);
        appendActiveDelta(responseText);
        publishActiveStage(ChatActivityStage::Finalizing);
        finishActiveResponse(outcome.result.success
                                 ? ChatMessageStatus::Complete
                                 : ChatMessageStatus::Failed,
                             outcome.result.success
                                 ? QString()
                                 : outcome.result.errorMessage);
        return true;
    }

    finishActiveResponse(ChatMessageStatus::Failed,
                         QStringLiteral("unsupported route type"));
    return true;
}

ChatPreparationRequest AIBrain::makeChatPreparationRequest(
    const QString& requestId,
    quint64 generation,
    const QString& reason,
    const QString& triggerTag,
    const QString& sessionId) const {
    ChatPreparationRequest request;
    request.requestId = requestId;
    request.generation = generation;
    request.sessionId = sessionId;
    request.reason = reason;
    request.triggerTag = triggerTag;
    request.petName = m_petName;
    request.allowedActions = allowedActionsForTrigger(triggerTag);
    // The current user message is already remembered for the next turn; this
    // request receives it once via reason, alongside the prior conversation.
    request.conversationMemory = m_activeDialogueResponse
        ? m_activeDialogueResponse->priorConversation : m_memory;
    request.workingMemory = m_workingMemoryCache.all();
    request.skills = m_skillStore.all();
    request.emotion = currentEmotionSnapshot();
    request.runtimeMetadata = m_chatPreparationRuntimeMetadata;
    request.identityBaseline = m_identityBaseline;
    request.personalityPolicy = m_personalityPolicy;
    request.promptTemplate = m_promptTemplate;
    return request;
}

ToolPolicyContext AIBrain::buildToolPolicyContext(const QString& triggerTag,
                                                  const QString& userInput,
                                                  bool initiatedByLlm) const {
    const AiToolAccessPolicy& toolAccessPolicy = ConfigManager::instance().getAiToolAccessPolicy();

    ToolPolicyContext context;
    context.triggerTag = triggerTag;
    context.userInput = userInput;
    context.initiatedByLlm = initiatedByLlm;
    context.allowedRootPaths = toolAccessPolicy.allowedRoots;
    context.grantedToolNames = toolAccessPolicy.autoGrantedTools;
    if (context.allowedRootPaths.isEmpty()) {
        context.allowedRootPaths.append(QCoreApplication::applicationDirPath());
        context.allowedRootPaths.append(QDir::currentPath());
    }
    return context;
}

void AIBrain::rememberToolOutcome(const QString& toolName,
                                  const QString& triggerTag,
                                  bool initiatedByLlm,
                                  const ToolExecutionOutcome& outcome,
                                  const QString& sessionId) {
    QJsonObject runtimeEvent{
        {QStringLiteral("toolName"), toolName},
        {QStringLiteral("success"), outcome.result.success},
        {QStringLiteral("resultSummary"), outcome.result.success
             ? QStringLiteral("tool execution succeeded")
             : QStringLiteral("tool execution failed")}
    };
    if (!outcome.result.success) {
        runtimeEvent.insert(QStringLiteral("errorCode"),
                            outcome.executed
                                ? QStringLiteral("TOOL_EXECUTION_FAILED")
                                : QStringLiteral("TOOL_NOT_EXECUTED"));
    }
    appendRuntimeEvent(QStringLiteral("ToolExecutionCompleted"), sessionId,
                       runtimeEvent);

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

    MemoryEntry toolMemory;
    toolMemory.type = MemoryType::Event;
    toolMemory.key = QStringLiteral("tool_execution");
    toolMemory.value = event;
    toolMemory.tags = {triggerTag,
                       initiatedByLlm ? QStringLiteral("llm")
                                      : QStringLiteral("router"),
                       toolName};
    toolMemory.source = QStringLiteral("tool_result");
    MemoryMetadata::recordSession(toolMemory, sessionId);
    toolMemory.payload[QStringLiteral("request_id")] = outcome.requestId;
    // A tool context identifies the actual invocation, not every use of its name.
    toolMemory.payload[QStringLiteral("tool")] = outcome.requestId;
    toolMemory.payload[QStringLiteral("source_tags")] = QJsonArray::fromStringList(toolMemory.tags);
    annotateMemoryEntry(toolMemory);
    MemoryMutationBatch mutations;
    m_memoryStore.stageEntry(toolMemory, &mutations);
    if (m_chatSideEffectQueue && m_chatSideEffectQueue->isAccepting()) {
        DeferredChatSideEffect effect;
        effect.type = ChatSideEffectType::UserMemoryWrite;
        effect.requestId = outcome.requestId;
        effect.generation = m_activeDialogueResponse
            ? m_activeDialogueResponse->generation : m_requestGeneration;
        effect.sessionId = sessionId;
        effect.memoryMutations = mutations;
        if (!m_chatSideEffectQueue->tryEnqueue(std::move(effect))) {
            m_memoryStore.rollbackMutationBatch(mutations);
            qWarning() << "[AIBrain] unable to queue tool outcome memory";
        }
    } else {
        m_memoryStore.rollbackMutationBatch(mutations);
        qWarning() << "[AIBrain] tool outcome persistence queue is unavailable";
    }

    const QString summary = QStringLiteral("工具 %1 执行%2")
        .arg(toolName, outcome.result.success ? QStringLiteral("成功") : QStringLiteral("失败"));
    WorkingMemoryItem wm;
    wm.summary = summary;
    wm.content = summary + QStringLiteral("：")
        + m_toolRuntime.sanitizer()->toPayload(outcome.result).left(1200);
    if (MemoryExtractor::isLikelySensitiveContent(wm.content)) {
        wm.privacyLevel = PrivacyLevel::Sensitive;
    }
    wm.tags = {triggerTag, toolName};
    wm.source = QStringLiteral("tool_result");
    wm.importance = 0.2;
    m_workingMemoryCache.add(wm);
}

void AIBrain::restoreConversationHistory(const QList<ChatHistoryEntry>& history) {
    if (m_busy) return;
    m_memory.clear();
    m_workingMemoryCache.clear();
    QSet<QString> seen;
    QSet<QString> userIds;
    for (const ChatHistoryEntry& entry : history) {
        if (entry.status != ChatMessageStatus::Complete || entry.content.trimmed().isEmpty()
            || (!entry.id.isEmpty() && seen.contains(entry.id))) continue;
        if (entry.role == QLatin1String("user")) {
            userIds.insert(entry.id);
        } else if (entry.role != QLatin1String("assistant")
                   || (!entry.replyToId.isEmpty() && !userIds.contains(entry.replyToId))) {
            continue;
        }
        if (!entry.id.isEmpty()) seen.insert(entry.id);
        ChatMessage message;
        message.role = entry.role;
        message.content = entry.content;
        appendToMemory(message);
    }
}

void AIBrain::rememberScreenObservation(const QString& observation) {
    if (observation.trimmed().isEmpty()) return;
    m_workingMemoryCache.cleanup();
    WorkingMemoryItem item;
    item.summary = QStringLiteral("屏幕观察（可能不准确）：") + observation.left(600);
    item.content = item.summary;
    if (MemoryExtractor::isLikelySensitiveContent(item.content)) {
        item.privacyLevel = PrivacyLevel::Sensitive;
    }
    item.source = QStringLiteral("screen_observation");
    item.createdAt = QDateTime::currentDateTimeUtc();
    item.expiresAt = item.createdAt.addSecs(5 * 60);
    item.importance = 0.3;
    m_workingMemoryCache.add(item);
}

void AIBrain::appendToMemory(const ChatMessage& message) {
    // Tool protocol messages are only valid inside the request that contains
    // their matching assistant tool_calls entry. Cross-request memory keeps
    // natural conversation only, otherwise Gemini rejects orphan results.
    if ((message.role != QLatin1String("user") && message.role != QLatin1String("assistant"))
        || message.content.trimmed().isEmpty()
        || !message.toolCallId.isEmpty() || !message.toolCalls.isEmpty()) {
        return;
    }
    ChatMessage naturalMessage;
    naturalMessage.role = message.role;
    naturalMessage.content = message.content.left(8000);
    m_memory.append(naturalMessage);

    while (m_memory.size() > m_maxMemoryMessages) {
        // Trim the oldest turn, preserving user/assistant pairs when possible.
        m_memory.removeFirst();
        while (!m_memory.isEmpty() && m_memory.first().role != QLatin1String("user")
               && std::any_of(m_memory.cbegin(), m_memory.cend(), [](const ChatMessage& item) {
                   return item.role == QLatin1String("user");
               })) {
            m_memory.removeFirst();
        }
    }
}
