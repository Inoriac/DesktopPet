#include "tool_runtime.h"

#include <QJsonDocument>
#include <QUuid>
#include <QPointer>
#include <QCoreApplication>
#include <QTimer>

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
    : QObject(parent),m_worker(new QThreadPool) { m_worker->setMaxThreadCount(1); }

void ToolRuntime::setToolRegistry(ToolRegistry* registry) {
    if (m_registry != registry) {
        cancelAll();
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
        if (!pendingRequest.dispatchToken) pendingRequest.dispatchToken=std::make_shared<ToolDispatchToken>();
        pendingRequest.dispatchToken->bindingEnabled=m_registry->executionBinding(request.toolName).enabled;
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

    if (request.dispatchToken && !request.dispatchToken->begin()) {
        outcome.result=ToolResult::fail("Tool cancelled or binding changed before dispatch");
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

ToolRuntime::~ToolRuntime() {
    cancelAll();
    // Queued owner-thread events will be removed with this QObject. Settle those
    // calls independently; already-started and asynchronous work retains its receipt.
    for (const auto& call:m_calls) {
        if (!call->ownerQueued) continue;
        { QMutexLocker tokenLock(&call->token->mutex); if (!call->token->cancelled) continue; }
        { QMutexLocker lock(&call->mutex);
            if (call->received) continue;
            call->received=true; call->outcome.result=ToolResult::fail("Tool cancelled before runtime destruction");
        }
        auto complete=[call] {
            { QMutexLocker lock(&call->mutex); if (call->settled) return; call->settled=true; }
            if (call->completion) call->completion(std::move(call->outcome));
        };
        if (QCoreApplication::instance()) QMetaObject::invokeMethod(QCoreApplication::instance(),std::move(complete),Qt::QueuedConnection);
        else complete();
    }
    // In-flight work keeps its value callback; destroying a facade does not block the GUI.
    auto* pool=m_worker;
    if (!pool->activeThreadCount()) delete pool;
    else {
        auto* timer=new QTimer(QCoreApplication::instance()); timer->setInterval(25);
        connect(timer,&QTimer::timeout,timer,[pool,timer] { if (!pool->activeThreadCount()) { timer->stop(); delete pool; timer->deleteLater(); } }); timer->start();
    }
}
void ToolRuntime::cancel(const QString& id) { const auto call=m_calls.value(id); if (call) call->token->cancel(); }
void ToolRuntime::cancelAll() { for (const auto& call:m_calls) call->token->cancel(); }
void ToolRuntime::settle(const std::shared_ptr<AsyncCall>& call) {
    { QMutexLocker lock(&call->mutex); if (call->settled || !call->received) return; call->settled=true; }
    m_calls.remove(call->outcome.requestId);
    call->outcome.result=m_sanitizer.sanitize(call->outcome.result);
    if (call->outcome.executed) emit toolExecutionFinished(call->outcome.requestId,call->outcome.toolName,call->outcome.result.success,m_sanitizer.toPayload(call->outcome.result));
    else emit toolExecutionBlocked(call->outcome.requestId,call->outcome.toolName,call->outcome.result.errorMessage);
    if (call->completion) call->completion(std::move(call->outcome));
}
void ToolRuntime::executeAsync(const ToolExecutionRequest& request,Completion completion) { executeAsyncImpl(request,std::move(completion),false); }
void ToolRuntime::resolveConfirmationAsync(const QString& id,bool approved,Completion completion) {
    auto it=m_pendingConfirmations.find(id);
    if (it==m_pendingConfirmations.end() || !approved) { if (completion) completion(resolveConfirmation(id,approved)); return; }
    auto request=it.value(); m_pendingConfirmations.erase(it); executeAsyncImpl(request,std::move(completion),true);
}
void ToolRuntime::executeAsyncImpl(ToolExecutionRequest request,Completion completion,bool confirmed) {
    if (request.requestId.isEmpty()) request.requestId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    ToolExecutionOutcome outcome; outcome.requestId=request.requestId; outcome.toolName=request.toolName;
    auto* tool=m_registry?m_registry->getTool(request.toolName):nullptr;
    if (!tool || !m_registry->supportsAsync(request.toolName)) {
        outcome.policyDecision=ToolPolicyDecision::deny(ToolRiskLevel::L4Dangerous,"Tool binding is unavailable");
        outcome.result=ToolResult::fail("Tool binding is unavailable"); if (completion) completion(outcome); return;
    }
    if (!request.dispatchToken) request.dispatchToken=std::make_shared<ToolDispatchToken>();
    if (!request.dispatchToken->bindingEnabled) request.dispatchToken->bindingEnabled=m_registry->executionBinding(request.toolName).enabled;
    if (!request.dispatchToken->allowed()) {
        outcome.result=ToolResult::fail("Tool cancelled or binding changed before dispatch"); if (completion) completion(outcome); return;
    }
    outcome.policyDecision=policyEngine()->evaluate(*tool,request.arguments,request.policyContext);
    if (outcome.policyDecision.isDenied()) { outcome.result=ToolResult::fail(outcome.policyDecision.reason); if (completion) completion(outcome); return; }
    if (outcome.policyDecision.needsConfirmation() && !confirmed) {
        while (m_pendingConfirmations.contains(request.requestId)) request.requestId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        outcome.requestId=request.requestId; m_pendingConfirmations.insert(request.requestId,request);
        outcome.result=ToolResult::fail(outcome.policyDecision.reason);
        emit toolConfirmationRequired(request.requestId,request.toolName,outcome.policyDecision.reason,request.arguments);
        if (completion) completion(outcome); return;
    }
    if (m_calls.contains(request.requestId)) { outcome.result=ToolResult::fail("Duplicate tool request identity"); if (completion) completion(outcome); return; }
    auto call=std::make_shared<AsyncCall>(); call->completion=std::move(completion); call->outcome=outcome;
    if (request.dispatchToken) call->token=request.dispatchToken; else request.dispatchToken=call->token;
    m_calls.insert(request.requestId,call);
    const auto binding=m_registry->executionBinding(request.toolName); QPointer<ToolRuntime> guard(this);
    auto done=[guard,call,id=request.requestId,name=request.toolName](ToolExecutionOutcome value) mutable {
        { QMutexLocker lock(&call->mutex); if (call->received) return; value.requestId=id; value.toolName=name; call->outcome=std::move(value); call->received=true; }
        auto settle=[guard,call]() mutable {
            if (guard) { guard->settle(call); return; }
            { QMutexLocker lock(&call->mutex); if (call->settled) return; call->settled=true; }
            if (call->completion) call->completion(std::move(call->outcome));
        };
        if (QCoreApplication::instance()) QMetaObject::invokeMethod(QCoreApplication::instance(),std::move(settle),Qt::QueuedConnection);
        else settle();
    };
    emit toolExecutionStarted(request.requestId,request.toolName);
    if (binding.metadata.executor==HostToolExecutor::Async) {
        if (!binding.enabled->load() || !call->token->allowed()) { outcome.result=ToolResult::fail("Tool cancelled before dispatch"); done(outcome); return; }
        try { binding.async(request,done); } catch (...) { outcome.executed=true; outcome.result=ToolResult::fail("Asynchronous tool raised an exception; outcome unknown"); done(outcome); }
        return;
    }
    auto run=[call,binding,request,outcome,done](AITool* instance) mutable {
        auto value=outcome;
        if (!binding.enabled->load() || !call->token->begin()) value.result=ToolResult::fail("Tool was cancelled before dispatch");
        else {
            try {
                if (!instance || !instance->validate(request.arguments)) value.result=ToolResult::fail("Invalid tool parameters");
                else { value.executed=true; value.result=instance->execute(request.arguments); }
            } catch (...) { value.result=ToolResult::fail("Tool raised an exception; outcome unknown"); }
        }
        done(std::move(value));
    };
    if (binding.metadata.executor==HostToolExecutor::OwnerThread) {
        call->ownerQueued=true;
        // A registry switch cancels this token; never resolve an old binding in a new registry.
        QMetaObject::invokeMethod(this,[call,binding,tool,run=std::move(run)]() mutable { run(binding.enabled->load() && call->token->allowed()?tool:nullptr); },Qt::QueuedConnection);
        return;
    }
    m_worker->start([binding,run=std::move(run),outcome,done]() mutable {
        try { auto workerTool=binding.factory(); run(workerTool.get()); }
        catch (...) { auto failure=outcome; failure.result=ToolResult::fail("Tool factory failed"); done(failure); }
    });
}
