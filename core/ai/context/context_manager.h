#ifndef DESKTOP_PET_CONTEXT_MANAGER_H
#define DESKTOP_PET_CONTEXT_MANAGER_H

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "ai_types.h"
#include "context_budget.h"
#include "ai/context_builder.h"

struct AgentContextRequest {
    QString petName;
    QString taskInput;
    QString reason;
    QString currentState;
    QString triggerTag;
    QStringList allowedActions;
    QStringList memoryHints;
    QList<ChatMessage> shortTermMessages;
    QJsonArray availableTools;
    std::optional<PersonaProjection> personaProjection;
};

class ContextManager {
public:
    void setBudget(const ContextBudget& budget);
    const ContextBudget& budget() const { return m_budget; }

    // 注入通用提示词模版与独立身份基线（与 AIBrain 路径保持一致）。
    void setIdentityBaseline(const IdentityBaseline& baseline) { m_builder.setIdentityBaseline(baseline); }
    void setPromptTemplate(const PromptTemplate& templ) { m_builder.setPromptTemplate(templ); }

    QList<ChatMessage> buildMessages(const AgentContextRequest& request) const;
    QString sanitizeForLlm(const QString& text) const;

private:
    QString buildAgentTaskContext(const AgentContextRequest& request) const;

private:
    ContextBuilder m_builder;
    ContextBudget m_budget;
};

#endif // DESKTOP_PET_CONTEXT_MANAGER_H
