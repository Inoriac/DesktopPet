#ifndef DESKTOP_PET_TOOL_RUNTIME_H
#define DESKTOP_PET_TOOL_RUNTIME_H

#include <QObject>
#include <QThreadPool>
#include <functional>
#include <memory>
#include <atomic>
#include "ai/context/context_spec.h"
#include <QHash>
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
    int timeoutMs = 0;
    ContextSpec context;
    QString activityId;
    bool autonomous = false;
    std::shared_ptr<ToolDispatchToken> dispatchToken;
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
    ~ToolRuntime() override;
    using Completion=std::function<void(ToolExecutionOutcome)>;
    void executeAsync(const ToolExecutionRequest& request,Completion completion);
    void resolveConfirmationAsync(const QString& requestId,bool approved,Completion completion);
    void cancel(const QString& requestId);
    void cancelAll();

    void setToolRegistry(ToolRegistry* registry);
    void setPolicyEngine(PolicyEngine* policyEngine);

    ToolRegistry* toolRegistry() const { return m_registry; }
    PolicyEngine* policyEngine() const;
    ToolResultSanitizer* sanitizer() { return &m_sanitizer; }

    ToolExecutionOutcome execute(const ToolExecutionRequest& request);
    ToolExecutionOutcome resolveConfirmation(const QString& requestId, bool approved);
    void cancelPendingConfirmations(const QString& reason = QStringLiteral("Tool confirmation cancelled"));
    bool hasPendingConfirmation(const QString& requestId) const;

signals:
    void toolExecutionStarted(const QString& requestId, const QString& toolName);
    void toolExecutionFinished(const QString& requestId, const QString& toolName, bool success, const QString& payload);
    void toolExecutionBlocked(const QString& requestId, const QString& toolName, const QString& reason);
    void toolConfirmationRequired(const QString& requestId,
                                  const QString& toolName,
                                  const QString& reason,
                                  const QJsonObject& arguments);

private:
    void executeAsyncImpl(ToolExecutionRequest request,Completion completion,bool confirmed);
    ToolExecutionOutcome executeImpl(const ToolExecutionRequest& request, bool userConfirmed);

    struct AsyncCall { QMutex mutex; Completion completion; ToolExecutionOutcome outcome; std::shared_ptr<ToolDispatchToken> token=std::make_shared<ToolDispatchToken>(); bool received=false, settled=false, ownerQueued=false; };
    void settle(const std::shared_ptr<AsyncCall>& call);
    QThreadPool* m_worker = nullptr;
    QHash<QString,std::shared_ptr<AsyncCall>> m_calls;
    ToolRegistry* m_registry = nullptr;       // non-owning
    PolicyEngine m_defaultPolicyEngine;
    PolicyEngine* m_policyEngine = nullptr;   // non-owning, fallback to m_defaultPolicyEngine
    ToolResultSanitizer m_sanitizer;
    QHash<QString, ToolExecutionRequest> m_pendingConfirmations;
};

#endif // DESKTOP_PET_TOOL_RUNTIME_H
