#include "tool_runtime.h"

#include <QJsonDocument>
#include <QUuid>

QJsonObject ToolExecutionOutcome::toJson() const {
    QJsonObject obj;
    obj["request_id"] = requestId;
    obj["tool_name"] = toolName;
    obj["executed"] = executed;
    obj["policy"] = policyDecision.toJson();
    obj["result"] = result.toJson();
    return obj;
}

ToolRuntime::ToolRuntime(QObject* parent)
    : QObject(parent) {}

void ToolRuntime::setToolRegistry(ToolRegistry* registry) {
    m_registry = registry;
}

void ToolRuntime::setPolicyEngine(PolicyEngine* policyEngine) {
    m_policyEngine = policyEngine;
}

PolicyEngine* ToolRuntime::policyEngine() const {
    return m_policyEngine ? m_policyEngine : const_cast<PolicyEngine*>(&m_defaultPolicyEngine);
}

ToolExecutionOutcome ToolRuntime::execute(const ToolExecutionRequest& request) {
    ToolExecutionOutcome outcome;
    outcome.requestId = request.requestId.isEmpty()
                            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                            : request.requestId;
    outcome.toolName = request.toolName;

    if (!m_registry) {
        outcome.policyDecision = ToolPolicyDecision::deny(ToolRiskLevel::L4Dangerous, "ToolRegistry is not configured");
        outcome.result = ToolResult::fail(outcome.policyDecision.reason);
        emit toolExecutionBlocked(outcome.requestId, outcome.toolName, outcome.policyDecision.reason);
        return outcome;
    }

    AITool* tool = m_registry->getTool(request.toolName);
    if (!tool) {
        outcome.policyDecision = ToolPolicyDecision::deny(ToolRiskLevel::L4Dangerous, "Unknown tool");
        outcome.result = ToolResult::fail(QString("Unknown tool: %1").arg(request.toolName));
        emit toolExecutionBlocked(outcome.requestId, outcome.toolName, outcome.result.errorMessage);
        return outcome;
    }

    outcome.policyDecision = policyEngine()->evaluate(*tool, request.arguments, request.policyContext);
    if (outcome.policyDecision.needsConfirmation() && !request.userConfirmed) {
        outcome.result = ToolResult::fail(outcome.policyDecision.reason);
        emit toolConfirmationRequired(outcome.requestId, outcome.toolName, outcome.policyDecision.reason);
        return outcome;
    }

    if (outcome.policyDecision.isDenied()) {
        outcome.result = ToolResult::fail(outcome.policyDecision.reason);
        emit toolExecutionBlocked(outcome.requestId, outcome.toolName, outcome.policyDecision.reason);
        return outcome;
    }

    Q_UNUSED(request.timeoutMs)
    emit toolExecutionStarted(outcome.requestId, outcome.toolName);

    outcome.executed = true;
    outcome.result = m_sanitizer.sanitize(m_registry->executeTool(request.toolName, request.arguments));

    const QString payload = m_sanitizer.toPayload(outcome.result);
    emit toolExecutionFinished(outcome.requestId, outcome.toolName, outcome.result.success, payload);
    return outcome;
}