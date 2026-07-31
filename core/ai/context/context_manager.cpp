#include "context_manager.h"

#include <QJsonDocument>

#include "ai/prompt/prompt_renderer.h"

void ContextManager::setBudget(const ContextBudget& budget) {
    m_budget = budget;
}

QList<ChatMessage> ContextManager::buildMessages(const AgentContextRequest& request) const {
    QList<ChatMessage> messages;

    ChatMessage systemMessage;
    systemMessage.role = "system";
    systemMessage.content = m_builder.buildSystemPrompt(request.petName)
        + "\n\n你现在通过 AgentCore 工作。只在必要时调用工具；高风险工具必须等待用户确认。";
    messages.append(systemMessage);

    for (const ChatMessage& message : request.shortTermMessages) {
        messages.append(message);
    }

    ChatMessage taskMessage;
    taskMessage.role = "user";
    taskMessage.content = sanitizeForLlm(buildAgentTaskContext(request));
    messages.append(taskMessage);

    return m_budget.trimMessages(messages);
}

QString ContextManager::sanitizeForLlm(const QString& text) const {
    // 脱敏关键词与 PromptRenderer::redactSecrets 集中维护，避免认知漂移。
    QString sanitized = PromptRenderer::redactSecrets(text);
    return m_budget.trimString(sanitized);
}

QString ContextManager::buildAgentTaskContext(const AgentContextRequest& request) const {
    QString context = m_builder.buildRuntimeContext(request.petName,
                                                    request.reason,
                                                    request.currentState,
                                                    request.triggerTag,
                                                    request.allowedActions);
    context += "\n\n当前用户任务：" + request.taskInput;

    if (!request.memoryHints.isEmpty()) {
        context += "\n\n相关记忆：\n- " + request.memoryHints.join("\n- ");
    }

    if (!request.availableTools.isEmpty()) {
        context += "\n\n当前可用工具数量：" + QString::number(request.availableTools.size());
        QJsonArray limitedTools;
        const int limit = qMin(request.availableTools.size(), m_budget.maxToolSchemas);
        for (int i = 0; i < limit; ++i) {
            limitedTools.append(request.availableTools.at(i));
        }
        context += "\n工具摘要：" + QString::fromUtf8(QJsonDocument(limitedTools).toJson(QJsonDocument::Compact));
    }

    return context;
}