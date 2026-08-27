#include "llm_chat_model_client.h"

#include "llm_chat_service.h"

#include <utility>

LlmChatModelClient::LlmChatModelClient(LlmChatService* service)
    : m_service(service) {}

void LlmChatModelClient::completeOnce(const ModelRouteConfig& route,
                                      const QList<ChatMessage>& messages,
                                      const QJsonArray& tools,
                                      LlmCompletionHandler callback,
                                      const QString& petName) {
    if (!callback) return;
    if (!m_service) {
        callback(false, {}, QStringLiteral("LLM chat service is unavailable"));
        return;
    }
    LlmConfig config = route.llm;
    config.retryCount = 0;
    m_service->requestAsyncWithConfig(
        config, messages, tools, std::move(callback), petName);
}

std::shared_ptr<LlmRequestHandle> LlmChatModelClient::completeOnceStream(
    const ModelRouteConfig& route,
    const QList<ChatMessage>& messages,
    const QJsonArray& tools,
    LlmStreamObserver observer,
    LlmCompletionHandler completion,
    const QString& petName) {
    if (!completion) return {};
    if (!m_service) {
        completion(false, {}, QStringLiteral("LLM chat service is unavailable"));
        return {};
    }
    LlmConfig config = route.llm;
    config.retryCount = 0;
    return m_service->requestStreamAsyncWithConfig(
        config, messages, tools, std::move(observer), std::move(completion), petName);
}
