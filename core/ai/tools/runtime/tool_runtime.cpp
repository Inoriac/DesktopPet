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
    if (m_registry != registry) {
        cancelPendingConfirmations("Tool registry changed");
    }
    m_registry = registry;
}

void ToolRuntime::setPolicyEngine(PolicyEngine* policyEngine) {
    m_policyEngine = policyEngine;
}

PolicyEngine* ToolRuntime::policyEngine() const {
    return m_policyEngine ? m_policyEngine : const_cast<PolicyEngine*>(&m_defaultPolicyEngine);
}

ToolExecutionOutcome ToolRuntime::execute(const ToolExecutionRequest& request) {
    return executeImpl(request, false);
}

bool ToolRuntime::hasPendingConfirmation(const QString& requestId) const {
    return m_pendingConfirmations.contains(requestId);
}

void ToolRuntime::cancelPendingConfirmations(const QString& reason) {
    const auto pending = m_pendingConfirmations;
    m_pendingConfirmations.clear();
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        emit toolExecutionBlocked(it.key(), it.value().toolName, reason);
    }
}

ToolExecutionOutcome ToolRuntime::resolveConfirmation(const QString& requestId, bool approved) {
    const auto it = m_pendingConfirmations.find(requestId);
    if (it == m_pendingConfirmations.end()) {
        ToolExecutionOutcome outcome;
        outcome.requestId = requestId;
        outcome.policyDecision = ToolPolicyDecision::deny(
            ToolRiskLevel::L4Dangerous, "No pending confirmation for request");
        outcome.result = ToolResult::fail(outcome.policyDecision.reason);
        emit toolExecutionBlocked(requestId, {}, outcome.policyDecision.reason);
        return outcome;
    }

    const ToolExecutionRequest request = it.value();
    m_pendingConfirmations.erase(it);

    AITool* tool = m_registry ? m_registry->getTool(request.toolName) : nullptr;
    if (!approved) {
        ToolExecutionOutcome outcome;
        outcome.requestId = request.requestId;
        outcome.toolName = request.toolName;
        outcome.policyDecision = ToolPolicyDecision::deny(
            tool ? policyEngine()->riskLevelForTool(*tool) : ToolRiskLevel::L4Dangerous,
            "User rejected tool execution");
        outcome.result = ToolResult::fail(outcome.policyDecision.reason);
        emit toolExecutionBlocked(outcome.requestId, outcome.toolName, outcome.policyDecision.reason);
        return outcome;
    }

    if (!tool) {
        ToolExecutionOutcome outcome;
        outcome.requestId = request.requestId;
        outcome.toolName = request.toolName;
        outcome.policyDecision = ToolPolicyDecision::deny(
            ToolRiskLevel::L4Dangerous, "Tool is no longer available");
        outcome.result = ToolResult::fail(outcome.policyDecision.reason);
        emit toolExecutionBlocked(outcome.requestId, outcome.toolName, outcome.policyDecision.reason);
        return outcome;
    }

    return executeImpl(request, true);
}

ToolExecutionOutcome ToolRuntime::executeImpl(const ToolExecutionRequest& request, bool userConfirmed) {
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
    if (outcome.policyDecision.needsConfirmation() && !userConfirmed) {
        while (m_pendingConfirmations.contains(outcome.requestId)) {
            outcome.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        ToolExecutionRequest pendingRequest = request;
        pendingRequest.requestId = outcome.requestId;
        m_pendingConfirmations.insert(outcome.requestId, pendingRequest);
        outcome.result = ToolResult::fail(outcome.policyDecision.reason);
        emit toolConfirmationRequired(outcome.requestId,
                                      outcome.toolName,
                                      outcome.policyDecision.reason,
                                      request.arguments);
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
