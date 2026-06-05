#include "tool_policy.h"

QString toolRiskLevelToString(ToolRiskLevel level) {
    switch (level) {
    case ToolRiskLevel::L0SafeRead:
        return "L0";
    case ToolRiskLevel::L1LocalQuery:
        return "L1";
    case ToolRiskLevel::L2LowRiskAction:
        return "L2";
    case ToolRiskLevel::L3HighRiskAction:
        return "L3";
    case ToolRiskLevel::L4Dangerous:
        return "L4";
    }
    return "L4";
}

QString toolPolicyActionToString(ToolPolicyAction action) {
    switch (action) {
    case ToolPolicyAction::Allow:
        return "Allow";
    case ToolPolicyAction::RequireConfirmation:
        return "RequireConfirmation";
    case ToolPolicyAction::Deny:
        return "Deny";
    }
    return "Deny";
}

QJsonObject ToolPolicyDecision::toJson() const {
    QJsonObject obj;
    obj["action"] = toolPolicyActionToString(action);
    obj["risk_level"] = toolRiskLevelToString(riskLevel);
    obj["reason"] = reason;
    return obj;
}

ToolPolicyDecision ToolPolicyDecision::allow(ToolRiskLevel level, const QString& reason) {
    ToolPolicyDecision decision;
    decision.action = ToolPolicyAction::Allow;
    decision.riskLevel = level;
    decision.reason = reason;
    return decision;
}

ToolPolicyDecision ToolPolicyDecision::requireConfirmation(ToolRiskLevel level, const QString& reason) {
    ToolPolicyDecision decision;
    decision.action = ToolPolicyAction::RequireConfirmation;
    decision.riskLevel = level;
    decision.reason = reason;
    return decision;
}

ToolPolicyDecision ToolPolicyDecision::deny(ToolRiskLevel level, const QString& reason) {
    ToolPolicyDecision decision;
    decision.action = ToolPolicyAction::Deny;
    decision.riskLevel = level;
    decision.reason = reason;
    return decision;
}

void PolicyEngine::setToolRiskLevel(const QString& toolName, ToolRiskLevel level) {
    m_toolRiskOverrides.insert(toolName, level);
}

void PolicyEngine::clearToolRiskLevel(const QString& toolName) {
    m_toolRiskOverrides.remove(toolName);
}

ToolRiskLevel PolicyEngine::riskLevelForTool(const AITool& tool) const {
    const auto it = m_toolRiskOverrides.constFind(tool.name());
    if (it != m_toolRiskOverrides.constEnd()) {
        return it.value();
    }
    return inferRiskLevel(tool);
}

ToolPolicyDecision PolicyEngine::evaluate(const AITool& tool,
                                          const QJsonObject& arguments,
                                          const ToolPolicyContext& context) const {
    Q_UNUSED(arguments)

    if (context.grantedToolNames.contains(tool.name())) {
        return ToolPolicyDecision::allow(riskLevelForTool(tool), "tool explicitly granted");
    }

    const ToolRiskLevel level = riskLevelForTool(tool);
    switch (level) {
    case ToolRiskLevel::L0SafeRead:
        return ToolPolicyDecision::allow(level, "safe read-only tool");
    case ToolRiskLevel::L1LocalQuery:
        if (isLocalFileName(tool.name()) && context.allowedRootPaths.isEmpty()) {
            return ToolPolicyDecision::requireConfirmation(level, "local file query requires scoped roots");
        }
        return ToolPolicyDecision::allow(level, "local query in allowed scope");
    case ToolRiskLevel::L2LowRiskAction:
        return ToolPolicyDecision::allow(level, "low risk desktop-pet action");
    case ToolRiskLevel::L3HighRiskAction:
        return ToolPolicyDecision::requireConfirmation(level, "high risk tool requires user confirmation");
    case ToolRiskLevel::L4Dangerous:
        return ToolPolicyDecision::deny(level, "dangerous tool is denied by default");
    }
    return ToolPolicyDecision::deny(ToolRiskLevel::L4Dangerous, "unknown policy state");
}

ToolRiskLevel PolicyEngine::inferRiskLevel(const AITool& tool) const {
    const QString name = tool.name().toLower();
    if (isSensitiveOrDangerousName(name)) {
        return ToolRiskLevel::L4Dangerous;
    }
    if (isHighRiskName(name)) {
        return ToolRiskLevel::L3HighRiskAction;
    }
    if (isLocalFileName(name) || name.contains("search") || name.contains("read")) {
        return ToolRiskLevel::L1LocalQuery;
    }
    if (tool.category() == ToolCategory::Query) {
        return ToolRiskLevel::L0SafeRead;
    }
    return ToolRiskLevel::L2LowRiskAction;
}

bool PolicyEngine::isSensitiveOrDangerousName(const QString& toolName) const {
    return toolName.contains("delete")
        || toolName.contains("remove")
        || toolName.contains("format")
        || toolName.contains("credential")
        || toolName.contains("password")
        || toolName.contains("secret")
        || toolName.contains("token")
        || toolName.contains("shutdown")
        || toolName.contains("system_modify");
}

bool PolicyEngine::isHighRiskName(const QString& toolName) const {
    return toolName.contains("shell")
        || toolName.contains("execute")
        || toolName.contains("run_process")
        || toolName.contains("launch")
        || toolName.contains("file_write")
        || toolName.contains("write_file");
}

bool PolicyEngine::isLocalFileName(const QString& toolName) const {
    return toolName.startsWith("file_")
        || toolName.contains("list_directory")
        || toolName.contains("read_text");
}