#ifndef DESKTOP_PET_TOOL_POLICY_H
#define DESKTOP_PET_TOOL_POLICY_H

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "ai/ai_tool.h"

enum class ToolRiskLevel {
    L0SafeRead,
    L1LocalQuery,
    L2LowRiskAction,
    L3HighRiskAction,
    L4Dangerous
};

enum class ToolPolicyAction {
    Allow,
    RequireConfirmation,
    Deny
};

QString toolRiskLevelToString(ToolRiskLevel level);
QString toolPolicyActionToString(ToolPolicyAction action);

struct ToolPolicyContext {
    QString triggerTag;
    QString userInput;
    bool initiatedByLlm = true;
    QStringList allowedRootPaths;
    QStringList grantedToolNames;
};

struct ToolPolicyDecision {
    ToolPolicyAction action = ToolPolicyAction::Deny;
    ToolRiskLevel riskLevel = ToolRiskLevel::L4Dangerous;
    QString reason;

    bool isAllowed() const { return action == ToolPolicyAction::Allow; }
    bool needsConfirmation() const { return action == ToolPolicyAction::RequireConfirmation; }
    bool isDenied() const { return action == ToolPolicyAction::Deny; }

    QJsonObject toJson() const;

    static ToolPolicyDecision allow(ToolRiskLevel level, const QString& reason = QString());
    static ToolPolicyDecision requireConfirmation(ToolRiskLevel level, const QString& reason);
    static ToolPolicyDecision deny(ToolRiskLevel level, const QString& reason);
};

class PolicyEngine {
public:
    void setToolRiskLevel(const QString& toolName, ToolRiskLevel level);
    void clearToolRiskLevel(const QString& toolName);

    ToolRiskLevel riskLevelForTool(const AITool& tool) const;
    ToolPolicyDecision evaluate(const AITool& tool,
                                const QJsonObject& arguments,
                                const ToolPolicyContext& context) const;

private:
    ToolRiskLevel inferRiskLevel(const AITool& tool) const;
    bool isSensitiveOrDangerousName(const QString& toolName) const;
    bool isHighRiskName(const QString& toolName) const;
    bool isLocalFileName(const QString& toolName) const;

private:
    QHash<QString, ToolRiskLevel> m_toolRiskOverrides;
};

#endif // DESKTOP_PET_TOOL_POLICY_H