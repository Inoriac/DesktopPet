#ifndef DESKTOP_PET_TOOL_RUNTIME_H
#define DESKTOP_PET_TOOL_RUNTIME_H

#include <QObject>
#include <QJsonObject>
#include <QString>

#include "tool_policy.h"
#include "tool_result_sanitizer.h"
#include "ai/tool_registry.h"

struct ToolExecutionRequest {
    QString requestId;
    QString toolName;
    QJsonObject arguments;
    ToolPolicyContext policyContext;
    bool userConfirmed = false;
    int timeoutMs = 0;
};

struct ToolExecutionOutcome {
    QString requestId;
    QString toolName;
    bool executed = false;
    ToolPolicyDecision policyDecision;
    ToolResult result;

    QJsonObject toJson() const;
};

class ToolRuntime : public QObject {
    Q_OBJECT

public:
    explicit ToolRuntime(QObject* parent = nullptr);

    void setToolRegistry(ToolRegistry* registry);
    void setPolicyEngine(PolicyEngine* policyEngine);

    ToolRegistry* toolRegistry() const { return m_registry; }
    PolicyEngine* policyEngine() const;
    ToolResultSanitizer* sanitizer() { return &m_sanitizer; }

    ToolExecutionOutcome execute(const ToolExecutionRequest& request);

signals:
    void toolExecutionStarted(const QString& requestId, const QString& toolName);
    void toolExecutionFinished(const QString& requestId, const QString& toolName, bool success, const QString& payload);
    void toolExecutionBlocked(const QString& requestId, const QString& toolName, const QString& reason);
    void toolConfirmationRequired(const QString& requestId, const QString& toolName, const QString& reason);

private:
    ToolRegistry* m_registry = nullptr;       // non-owning
    PolicyEngine m_defaultPolicyEngine;
    PolicyEngine* m_policyEngine = nullptr;   // non-owning, fallback to m_defaultPolicyEngine
    ToolResultSanitizer m_sanitizer;
};

#endif // DESKTOP_PET_TOOL_RUNTIME_H