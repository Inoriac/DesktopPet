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

#include "configLoader/config_manager.h"
#include "tools/runtime/tool_policy.h"
#include "memory/working_memory_cache.h"
#include "skill/skill_matcher.h"

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
        const QString payload = m_toolRuntime.sanitizer()->toPayload(outcome.result);
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

    WorkingMemoryItem wm;
    wm.summary = content.left(120);
    wm.content = content;
    wm.tags = {triggerTag, QStringLiteral("assistant")};
    wm.source = QStringLiteral("assistant_response");
    wm.importance = 0.3;
    m_workingMemoryCache.add(wm);
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

    const QString summary = QStringLiteral("工具 %1 执行%2")
        .arg(toolName, outcome.result.success ? QStringLiteral("成功") : QStringLiteral("失败"));
    WorkingMemoryItem wm;
    wm.summary = summary;
    wm.content = summary;
    wm.tags = {triggerTag, toolName};
    wm.source = QStringLiteral("tool_result");
    wm.importance = 0.2;
    m_workingMemoryCache.add(wm);
}

QList<ChatMessage> AIBrain::buildBaseMessages(const QString& reason,
                                              const QString& triggerTag) {
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
    const QStringList memoryHints = retrieveMemoryHints(reason, triggerTag);
    if (!memoryHints.isEmpty()) {
        contextMessage.content += "\n相关记忆：\n" + memoryHints.join("\n");
        contextMessage.content += "\n约束：不要编造未保存的历史；查询提醒时优先调用 schedule_list 获取真实任务状态；敏感记忆未经确认不得主动暴露。\n";
    }

    const QList<MatchedSkill> matchedSkills = m_skillMatcher.match(m_skillStore, reason, 2);
    if (!matchedSkills.isEmpty()) {
        const QStringList skillHints = m_skillMatcher.formatForContext(matchedSkills);
        contextMessage.content += "\n已学习的相关技能（可参考但不必严格遵循，按实际情况灵活运用）：\n"
                                  + skillHints.join("\n") + "\n";
    }
    messages.append(contextMessage);

    return messages;
}

QStringList AIBrain::retrieveMemoryHints(const QString& reason,
                                         const QString& triggerTag,
                                         int limit) {
    m_workingMemoryCache.cleanup(&m_memoryStore);

    MemoryQuery query;
    query.text = reason;
    query.limit = limit;
    query.includeSensitive = false;
    query.includeInactive = false;

    if (triggerTag == "user_request" || triggerTag == "manual") {
        query.preferredTypes = {
            MemoryType::Preference,
            MemoryType::Semantic,
            MemoryType::TaskShadow,
            MemoryType::Core,
            MemoryType::Relationship,
            MemoryType::Episodic
        };
    } else if (triggerTag == "proactive_chat") {
        query.preferredTypes = {
            MemoryType::Preference,
            MemoryType::Relationship,
            MemoryType::Core
        };
    } else {
        query.preferredTypes = {
            MemoryType::Preference,
            MemoryType::Core
        };
        query.limit = qMin(limit, 4);
    }

    return m_memoryRetriever.formatForContext(m_memoryRetriever.retrieve(m_memoryStore, query, &m_workingMemoryCache, m_embeddingIndex));
}

void AIBrain::appendToMemory(const ChatMessage& message) {
    m_memory.append(message);

    while (m_memory.size() > m_maxMemoryMessages) {
        int removeIndex = 0;
        for (int i = 0; i < m_memory.size(); ++i) {
            const ChatMessage& candidate = m_memory.at(i);
            if (candidate.role != "system" && candidate.role != "user") {
                removeIndex = i;
                break;
            }
        }
        m_memory.removeAt(removeIndex);
    }
}

