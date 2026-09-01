#include "model_router.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>

#include <memory>
#include <optional>
#include <atomic>

namespace {

QString circuitKey(ModelRole role, const QString& routeId) {
    return QString::number(static_cast<int>(role)) + QLatin1Char(':') + routeId;
}

bool isTransientProviderError(const QString& error) {
    const QString normalized = error.toLower();
    if (normalized.contains(QStringLiteral("cancelled"))
        || normalized.contains(QStringLiteral("canceled"))) {
        return false;
    }

    static const QRegularExpression httpStatusPattern(
        QStringLiteral("\\bhttp\\s+(\\d{3})\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch statusMatch =
        httpStatusPattern.match(error);
    if (statusMatch.hasMatch()) {
        const int status = statusMatch.captured(1).toInt();
        return status == 408 || status == 425 || status == 429
            || (status >= 500 && status <= 599);
    }

    return normalized.contains(QStringLiteral("timeout"))
        || normalized.contains(QStringLiteral("timed out"))
        || normalized.contains(QStringLiteral("429"))
        || normalized.contains(QStringLiteral("rate limit"))
        || normalized.contains(QStringLiteral("network"));
}

bool valueMatchesType(const QJsonValue& value, const QString& type) {
    if (type == QLatin1String("string")) return value.isString();
    if (type == QLatin1String("number")) return value.isDouble();
    if (type == QLatin1String("boolean")) return value.isBool();
    if (type == QLatin1String("object")) return value.isObject();
    if (type == QLatin1String("array")) return value.isArray();
    return false;
}

bool responseMatchesSchema(const QString& content,
                           const QJsonObject& schema) {
    if (schema.isEmpty()) return true;
    if (schema.value(QStringLiteral("type")).toString() != QLatin1String("object")) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(content.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonObject object = document.object();

    const QJsonArray required = schema.value(QStringLiteral("required")).toArray();
    for (const QJsonValue& field : required) {
        if (!field.isString() || !object.contains(field.toString())) {
            return false;
        }
    }

    const QJsonObject properties =
        schema.value(QStringLiteral("properties")).toObject();
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        if (!object.contains(it.key())) continue;
        if (!it.value().isObject()) return false;
        const QString type = it.value().toObject()
                                 .value(QStringLiteral("type")).toString();
        if (!valueMatchesType(object.value(it.key()), type)) return false;
    }
    return true;
}

ChatMessage repairInstruction() {
    ChatMessage message;
    message.role = QStringLiteral("user");
    message.content = QStringLiteral(
        "上一次输出不符合约定的 JSON Object Schema。"
        "请只返回满足 required 字段和基本类型约束的 JSON 对象，不要附加解释。");
    return message;
}

} // namespace

std::shared_ptr<LlmRequestHandle> ModelCompletionClient::completeOnceStream(
    const ModelRouteConfig& route,
    const QList<ChatMessage>& messages,
    const QJsonArray& tools,
    LlmStreamObserver observer,
    LlmCompletionHandler completion,
    const QString& petName) {
    auto handle = std::make_shared<llm_client_detail::LegacyRequestHandle>();
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (observer) {
        observer({LlmStreamEventType::Started, requestId,
                  ChatActivityStage::WaitingForModel, {}});
    }
    completeOnce(route, messages, tools,
        [handle, observer = std::move(observer), completion = std::move(completion),
         requestId](bool ok, LlmResponse response, QString error) mutable {
            if (handle->terminal.exchange(true)) return;
            if (handle->cancelled.load()) {
                if (completion) {
                    completion(false, {},
                               QStringLiteral("LLM_REQUEST_CANCELLED: request cancelled"));
                }
                return;
            }
            if (ok && observer && !response.content.isEmpty()) {
                observer({LlmStreamEventType::StageChanged, requestId,
                          ChatActivityStage::StreamingText, {}});
                observer({LlmStreamEventType::TextDelta, requestId,
                          ChatActivityStage::StreamingText, response.content});
            }
            if (completion) completion(ok, std::move(response), std::move(error));
        }, petName);
    return handle;
}

namespace {

class RoutedRequestHandle final : public LlmRequestHandle {
public:
    void cancel() override {
        m_cancelled.store(true);
        const auto active = m_active;
        if (active) active->cancel();
    }

    bool isCancelled() const override { return m_cancelled.load(); }

    void setActive(std::shared_ptr<LlmRequestHandle> active, int attemptIndex) {
        if (attemptIndex < m_activeAttemptIndex) return;
        m_activeAttemptIndex = attemptIndex;
        m_active = std::move(active);
        if (m_cancelled.load() && m_active) m_active->cancel();
    }

private:
    std::atomic_bool m_cancelled{false};
    std::shared_ptr<LlmRequestHandle> m_active;
    int m_activeAttemptIndex = -1;
};

} // namespace

ModelRouter::ModelRouter(const ModelRoleRegistry* registry,
                         ModelCompletionClient* client)
    : m_registry(registry), m_client(client) {}

ModelRouter::~ModelRouter() {
    *m_alive = false;
}

struct ModelRouter::CompletionState {
    ModelRequest request;
    ModelRoleConfig config;
    ModelCompletionHandler callback;
    bool sawInvalidOutput = false;
    std::optional<ModelRouteConfig> lastRoute;
    LlmStreamObserver observer;
    std::shared_ptr<RoutedRequestHandle> requestHandle;
    QList<LlmStreamEvent> bufferedEvents;
    bool visibleTextPublished = false;
    bool terminal = false;
    QString lastProviderError;
};

bool ModelRouter::routeMeetsConstraints(
    const ModelRouteConfig& route,
    const ModelLimits& limits,
    const ModelConstraints& constraints) const {
    if (!route.enabled || !route.llm.enabled
        || route.routeId.trimmed().isEmpty()
        || route.llm.provider.trimmed().isEmpty()
        || route.llm.baseUrl.trimmed().isEmpty()
        || route.llm.apiKey.trimmed().isEmpty()
        || route.llm.model.trimmed().isEmpty()) {
        return false;
    }
    if (constraints.requiresVision && !route.supportsVision) return false;

    const int maxLatencyMs = constraints.maxLatencyMs > 0
        ? constraints.maxLatencyMs : limits.maxLatencyMs;
    if (maxLatencyMs > 0 && route.llm.timeoutMs > maxLatencyMs) return false;

    const qint64 maxCost = constraints.maxEstimatedCostMicros > 0
        ? constraints.maxEstimatedCostMicros : limits.maxEstimatedCostMicros;
    if (maxCost > 0 && route.estimatedCostMicros > maxCost) return false;
    return true;
}

bool ModelRouter::isCircuitOpen(ModelRole role, const QString& routeId) const {
    const QString key = circuitKey(role, routeId);
    const auto it = m_openCircuitUntilMs.constFind(key);
    if (it == m_openCircuitUntilMs.constEnd()) return false;
    if (it.value() <= QDateTime::currentMSecsSinceEpoch()) {
        m_openCircuitUntilMs.remove(key);
        return false;
    }
    return true;
}

void ModelRouter::openCircuit(ModelRole role, const QString& routeId) {
    m_openCircuitUntilMs.insert(
        circuitKey(role, routeId),
        QDateTime::currentMSecsSinceEpoch() + 30 * 1000);
}

Result<ModelRouteConfig, DomainError> ModelRouter::resolve(
    ModelRole role, const ModelConstraints& constraints) const {
    if (!m_registry) {
        return Result<ModelRouteConfig, DomainError>::failure(
            domainError(QStringLiteral("MODEL_ROLE_UNAVAILABLE"),
                        QStringLiteral("Model role registry is unavailable")));
    }
    const auto configResult = m_registry->configFor(role);
    if (!configResult.isOk()) {
        return Result<ModelRouteConfig, DomainError>::failure(configResult.error());
    }
    const ModelRoleConfig& config = configResult.value();
    for (const ModelRouteConfig& route : config.routes) {
        if (!isCircuitOpen(role, route.routeId)
            && routeMeetsConstraints(route, config.limits, constraints)) {
            return Result<ModelRouteConfig, DomainError>::success(route);
        }
    }
    return Result<ModelRouteConfig, DomainError>::failure(
        domainError(QStringLiteral("MODEL_ROLE_UNAVAILABLE"),
                    QStringLiteral("No configured model route satisfies the request")));
}

void ModelRouter::completeAsync(const ModelRequest& request,
                                ModelCompletionHandler callback) {
    if (!callback) return;
    if (!m_registry || !m_client) {
        callback(Result<ModelCompletion, DomainError>::failure(
            domainError(QStringLiteral("MODEL_ROLE_UNAVAILABLE"),
                        QStringLiteral("Model router dependencies are unavailable"))));
        return;
    }

    const auto configResult = m_registry->configFor(request.role);
    if (!configResult.isOk()) {
        callback(Result<ModelCompletion, DomainError>::failure(configResult.error()));
        return;
    }
    auto state = std::make_shared<CompletionState>();
    state->request = request;
    state->config = configResult.value();
    state->callback = std::move(callback);
    attemptRoute(state, 0, request.messages, false);
}

std::shared_ptr<LlmRequestHandle> ModelRouter::completeStreamAsync(
    const ModelRequest& request,
    LlmStreamObserver observer,
    ModelCompletionHandler completion) {
    auto requestHandle = std::make_shared<RoutedRequestHandle>();
    if (!completion) return requestHandle;
    if (!m_registry || !m_client) {
        completion(Result<ModelCompletion, DomainError>::failure(
            domainError(QStringLiteral("MODEL_ROLE_UNAVAILABLE"),
                        QStringLiteral("Model router dependencies are unavailable"))));
        return requestHandle;
    }

    const auto configResult = m_registry->configFor(request.role);
    if (!configResult.isOk()) {
        completion(Result<ModelCompletion, DomainError>::failure(configResult.error()));
        return requestHandle;
    }

    auto state = std::make_shared<CompletionState>();
    state->request = request;
    state->config = configResult.value();
    state->callback = std::move(completion);
    state->observer = std::move(observer);
    state->requestHandle = requestHandle;
    attemptStreamRoute(state, 0);
    return requestHandle;
}

void ModelRouter::attemptStreamRoute(
    const std::shared_ptr<CompletionState>& state,
    int routeIndex) {
    if (state->terminal || state->requestHandle->isCancelled()) return;

    int selectedIndex = routeIndex;
    while (selectedIndex < state->config.routes.size()) {
        const ModelRouteConfig& candidate = state->config.routes.at(selectedIndex);
        if (!isCircuitOpen(state->request.role, candidate.routeId)
            && routeMeetsConstraints(candidate, state->config.limits,
                                     state->request.constraints)) {
            break;
        }
        ++selectedIndex;
    }
    if (selectedIndex >= state->config.routes.size()) {
        state->terminal = true;
        QJsonObject details;
        if (state->lastRoute.has_value()) {
            details.insert(QStringLiteral("routeId"), state->lastRoute->routeId);
            details.insert(QStringLiteral("provider"), state->lastRoute->llm.provider);
            details.insert(QStringLiteral("model"), state->lastRoute->llm.model);
        }
        const bool providerFailed = !state->lastProviderError.trimmed().isEmpty();
        state->callback(Result<ModelCompletion, DomainError>::failure(
            domainError(providerFailed
                            ? QStringLiteral("MODEL_PROVIDER_ERROR")
                            : QStringLiteral("MODEL_ROLE_UNAVAILABLE"),
                        providerFailed
                            ? state->lastProviderError
                            : QStringLiteral("No configured model route is currently available"),
                        details)));
        return;
    }

    const ModelRouteConfig selectedRoute = state->config.routes.at(selectedIndex);
    state->lastRoute = selectedRoute;
    state->bufferedEvents.clear();
    const std::shared_ptr<bool> alive = m_alive;
    auto attemptHandle = m_client->completeOnceStream(
        selectedRoute, state->request.messages, state->request.tools,
        [alive, state](const LlmStreamEvent& event) {
            if (!*alive || state->terminal || state->requestHandle->isCancelled()) {
                return;
            }
            const bool visibleDelta = event.type == LlmStreamEventType::TextDelta
                && !event.textDelta.isEmpty();
            if (!state->visibleTextPublished && !visibleDelta) {
                state->bufferedEvents.append(event);
                return;
            }
            if (visibleDelta && !state->visibleTextPublished) {
                state->visibleTextPublished = true;
                if (state->observer) {
                    for (const LlmStreamEvent& buffered : state->bufferedEvents) {
                        state->observer(buffered);
                    }
                }
                state->bufferedEvents.clear();
            }
            if (state->observer) state->observer(event);
        },
        [this, alive, state, selectedRoute, selectedIndex]
        (bool ok, LlmResponse response, QString error) mutable {
            if (!*alive || state->terminal) return;
            if (state->requestHandle->isCancelled()) {
                state->terminal = true;
                state->callback(Result<ModelCompletion, DomainError>::failure(
                    domainError(QStringLiteral("LLM_REQUEST_CANCELLED"),
                                QStringLiteral("Model request was cancelled"))));
                return;
            }
            if (!ok) {
                state->lastProviderError = error.trimmed().isEmpty()
                    ? QStringLiteral("Model provider request failed")
                    : error.trimmed();
                if (state->visibleTextPublished) {
                    state->terminal = true;
                    const QJsonObject details{
                        {QStringLiteral("routeId"), selectedRoute.routeId},
                        {QStringLiteral("provider"), selectedRoute.llm.provider},
                        {QStringLiteral("model"), selectedRoute.llm.model}
                    };
                    state->callback(Result<ModelCompletion, DomainError>::failure(
                        domainError(QStringLiteral("MODEL_STREAM_INTERRUPTED"),
                                    error.isEmpty()
                                        ? QStringLiteral("Model stream was interrupted")
                                        : error,
                                    details)));
                    return;
                }
                state->bufferedEvents.clear();
                if (isTransientProviderError(error)
                    && selectedIndex + 1 < state->config.routes.size()) {
                    openCircuit(state->request.role, selectedRoute.routeId);
                }
                attemptStreamRoute(state, selectedIndex + 1);
                return;
            }

            if (!state->visibleTextPublished && state->observer) {
                for (const LlmStreamEvent& buffered : state->bufferedEvents) {
                    state->observer(buffered);
                }
            }
            state->bufferedEvents.clear();
            state->terminal = true;
            ModelCompletion completion;
            completion.response = std::move(response);
            completion.dimensions.role = state->request.role;
            completion.dimensions.provider = selectedRoute.llm.provider;
            completion.dimensions.model = selectedRoute.llm.model;
            completion.dimensions.routeId = selectedRoute.routeId;
            completion.fallbackUsed = selectedIndex > 0;
            state->callback(Result<ModelCompletion, DomainError>::success(
                std::move(completion)));
        },
        state->request.petName);
    state->requestHandle->setActive(std::move(attemptHandle), selectedIndex);
}

void ModelRouter::attemptRoute(const std::shared_ptr<CompletionState>& state,
                               int routeIndex,
                               QList<ChatMessage> messages,
                               bool repairAttempt) {
    int selectedIndex = routeIndex;
    while (selectedIndex < state->config.routes.size()) {
        const ModelRouteConfig& candidate = state->config.routes.at(selectedIndex);
        if (!isCircuitOpen(state->request.role, candidate.routeId)
            && routeMeetsConstraints(candidate, state->config.limits,
                                     state->request.constraints)) {
            break;
        }
        ++selectedIndex;
    }
    if (selectedIndex >= state->config.routes.size()) {
        const QString code = state->sawInvalidOutput
            ? QStringLiteral("MODEL_OUTPUT_INVALID")
            : (!state->lastProviderError.trimmed().isEmpty()
                   ? QStringLiteral("MODEL_PROVIDER_ERROR")
                   : QStringLiteral("MODEL_ROLE_UNAVAILABLE"));
        QJsonObject details;
        if (state->lastRoute.has_value()) {
            details.insert(QStringLiteral("routeId"), state->lastRoute->routeId);
            details.insert(QStringLiteral("provider"), state->lastRoute->llm.provider);
            details.insert(QStringLiteral("model"), state->lastRoute->llm.model);
        }
        const QString message = state->sawInvalidOutput
            ? QStringLiteral("All routes returned output that did not match the required schema")
            : (!state->lastProviderError.trimmed().isEmpty()
                   ? state->lastProviderError
                   : QStringLiteral("No configured model route is currently available"));
        state->callback(Result<ModelCompletion, DomainError>::failure(
            domainError(code, message, details)));
        return;
    }

    const ModelRouteConfig selectedRoute = state->config.routes.at(selectedIndex);
    state->lastRoute = selectedRoute;
    const std::shared_ptr<bool> alive = m_alive;
    m_client->completeOnce(
        selectedRoute, messages, state->request.tools,
        [this, alive, state, selectedRoute, selectedIndex, messages,
         repairAttempt]
        (bool success, LlmResponse response, QString error) mutable {
            if (!*alive) return;
            if (!success) {
                state->lastProviderError = error.trimmed().isEmpty()
                    ? QStringLiteral("Model provider request failed")
                    : error.trimmed();
                if (isTransientProviderError(error)
                    && selectedIndex + 1 < state->config.routes.size()) {
                    openCircuit(state->request.role, selectedRoute.routeId);
                }
                attemptRoute(state, selectedIndex + 1,
                             state->request.messages, false);
                return;
            }

            if (!responseMatchesSchema(response.content,
                                       state->request.responseSchema)) {
                state->sawInvalidOutput = true;
                if (!repairAttempt) {
                    QList<ChatMessage> repairMessages = messages;
                    repairMessages.append(repairInstruction());
                    attemptRoute(state, selectedIndex,
                                 std::move(repairMessages), true);
                } else {
                    attemptRoute(state, selectedIndex + 1,
                                 state->request.messages, false);
                }
                return;
            }

            ModelCompletion completion;
            completion.response = std::move(response);
            completion.dimensions.role = state->request.role;
            completion.dimensions.provider = selectedRoute.llm.provider;
            completion.dimensions.model = selectedRoute.llm.model;
            completion.dimensions.routeId = selectedRoute.routeId;
            completion.fallbackUsed = selectedIndex > 0;
            state->callback(Result<ModelCompletion, DomainError>::success(
                std::move(completion)));
        },
        state->request.petName);
}
