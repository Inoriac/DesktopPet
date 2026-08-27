#ifndef DESKTOP_PET_MODEL_ROUTER_H
#define DESKTOP_PET_MODEL_ROUTER_H

#include <QHash>

#include <functional>
#include <memory>

#include "ai/domain/domain_result.h"
#include "ai/llm/llm_client.h"
#include "model_role_registry.h"

using ModelCompletionHandler =
    std::function<void(Result<ModelCompletion, DomainError>)>;

class ModelCompletionClient {
public:
    virtual ~ModelCompletionClient() = default;

    virtual void completeOnce(const ModelRouteConfig& route,
                              const QList<ChatMessage>& messages,
                              const QJsonArray& tools,
                              LlmCompletionHandler callback,
                              const QString& petName) = 0;

    virtual std::shared_ptr<LlmRequestHandle> completeOnceStream(
        const ModelRouteConfig& route,
        const QList<ChatMessage>& messages,
        const QJsonArray& tools,
        LlmStreamObserver observer,
        LlmCompletionHandler completion,
        const QString& petName);
};

class ModelRouter {
public:
    ModelRouter(const ModelRoleRegistry* registry,
                ModelCompletionClient* client);
    ~ModelRouter();

    void completeAsync(const ModelRequest& request,
                       ModelCompletionHandler callback);
    std::shared_ptr<LlmRequestHandle> completeStreamAsync(
        const ModelRequest& request,
        LlmStreamObserver observer,
        ModelCompletionHandler completion);
    Result<ModelRouteConfig, DomainError> resolve(
        ModelRole role, const ModelConstraints& constraints) const;

private:
    struct CompletionState;

    void attemptRoute(const std::shared_ptr<CompletionState>& state,
                      int routeIndex,
                      QList<ChatMessage> messages,
                      bool repairAttempt);
    void attemptStreamRoute(const std::shared_ptr<CompletionState>& state,
                            int routeIndex);
    bool routeMeetsConstraints(const ModelRouteConfig& route,
                               const ModelLimits& limits,
                               const ModelConstraints& constraints) const;
    bool isCircuitOpen(ModelRole role, const QString& routeId) const;
    void openCircuit(ModelRole role, const QString& routeId);

    const ModelRoleRegistry* m_registry = nullptr;
    ModelCompletionClient* m_client = nullptr;
    mutable QHash<QString, qint64> m_openCircuitUntilMs;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

#endif // DESKTOP_PET_MODEL_ROUTER_H
