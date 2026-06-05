#ifndef DESKTOP_PET_CONTEXT_MANAGER_H
#define DESKTOP_PET_CONTEXT_MANAGER_H

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

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
};

class ContextManager {
public:
    void setBudget(const ContextBudget& budget);
    const ContextBudget& budget() const { return m_budget; }

    QList<ChatMessage> buildMessages(const AgentContextRequest& request) const;
    QString sanitizeForLlm(const QString& text) const;

private:
    QString buildAgentTaskContext(const AgentContextRequest& request) const;

private:
    ContextBuilder m_builder;
    ContextBudget m_budget;
};

#endif // DESKTOP_PET_CONTEXT_MANAGER_H