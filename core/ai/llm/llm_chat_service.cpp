//
// LLM 聊天服务实现
//

#include "llm_chat_service.h"

#include <QDebug>
#include <memory>

#include "configLoader/config_manager.h"
#include "statistic_manager.h"

LlmChatService::LlmChatService(std::shared_ptr<LlmClient> client)
    : m_clientOverride(std::move(client)) {
    if (!m_clientOverride) {
        m_openAiClient = std::make_shared<OpenAICompatibleClient>();
        m_anthropicClient = std::make_shared<AnthropicMessagesClient>();
    }
}

std::shared_ptr<LlmClient> LlmChatService::clientForConfig(const LlmConfig& cfg) const {
    if (m_clientOverride) {
        return m_clientOverride;
    }
    const QString provider = cfg.provider.trimmed().toLower();
    if (provider == QStringLiteral("anthropic") ||
        provider == QStringLiteral("anthropic-messages")) {
        return m_anthropicClient;
    }
    if (provider.isEmpty() || provider == QStringLiteral("openai") ||
        provider == QStringLiteral("openai-compatible") ||
        provider == QStringLiteral("openai_compatible")) {
        return m_openAiClient;
    }
    return {};
}

void LlmChatService::requestAsync(const QList<ChatMessage>& messages,
                                  const QJsonArray& tools,
                                  LlmCompletionHandler callback,
                                  const QString& petName) {
    const LlmConfig& cfg = ConfigManager::instance().getLlmConfig();
    requestAsyncWithConfig(cfg, messages, tools, std::move(callback), petName);
}

void LlmChatService::requestAsyncWithConfig(const LlmConfig& cfg,
                                            const QList<ChatMessage>& messages,
                                            const QJsonArray& tools,
                                            LlmCompletionHandler callback,
                                            const QString& petName) {
    const auto selectedClient = clientForConfig(cfg);
    if (!selectedClient) {
        callback(false, {}, "LLM_PROVIDER_UNSUPPORTED: no client registered for provider");
        return;
    }

    if (!cfg.enabled) {
        callback(false, {}, "LLM disabled by config (aiSettings.enabled=false)");
        return;
    }

    const int attempts = qMax(1, cfg.retryCount + 1);
    auto attemptIndex = std::make_shared<int>(0);
    auto client = selectedClient;
    auto retryInvoker = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakRetryInvoker = retryInvoker;

    *retryInvoker = [client, cfg, messages, tools, callback, attempts, attemptIndex, weakRetryInvoker, petName]() mutable {
        const auto keepAlive = weakRetryInvoker.lock();
        if (!keepAlive) {
            return;
        }
        client->sendChatCompletionAsync(cfg, messages, tools,
            [callback, attempts, attemptIndex, keepAlive, petName]
            (bool ok, LlmResponse response, QString error) mutable {
                if (ok) {
                    const QString statsPetName = petName.isEmpty() ? QString("AI_GLOBAL") : petName;
                    StatisticManager::getInstance().recordLlmCall(
                        statsPetName, true, response.usage);
                    callback(true, std::move(response), {});
                    return;
                }

                *attemptIndex += 1;
                qWarning() << "[LlmChatService] Request failed, attempt" << *attemptIndex
                           << "/" << attempts << ", reason:" << error;

                if (*attemptIndex < attempts) {
                    (*keepAlive)();
                    return;
                }

                const QString statsPetName = petName.isEmpty()
                    ? QStringLiteral("AI_GLOBAL") : petName;
                StatisticManager::getInstance().recordLlmCall(
                    statsPetName, false, response.usage);
                callback(false, {}, error);
            });
    };

    (*retryInvoker)();
}

std::shared_ptr<LlmRequestHandle> LlmChatService::requestStreamAsync(
    const QList<ChatMessage>& messages,
    const QJsonArray& tools,
    LlmStreamObserver observer,
    LlmCompletionHandler completion,
    const QString& petName) {
    return requestStreamAsyncWithConfig(ConfigManager::instance().getLlmConfig(),
                                        messages, tools, std::move(observer),
                                        std::move(completion), petName);
}

std::shared_ptr<LlmRequestHandle> LlmChatService::requestStreamAsyncWithConfig(
    const LlmConfig& cfg,
    const QList<ChatMessage>& messages,
    const QJsonArray& tools,
    LlmStreamObserver observer,
    LlmCompletionHandler completion,
    const QString& petName) {
    const auto selectedClient = clientForConfig(cfg);
    if (!selectedClient) {
        auto handle = std::make_shared<llm_client_detail::LegacyRequestHandle>();
        handle->terminal.store(true);
        if (completion) {
            completion(false, {}, QStringLiteral("LLM_PROVIDER_UNSUPPORTED: no client registered for provider"));
        }
        return handle;
    }

    return selectedClient->sendChatCompletionStreamAsync(
        cfg, messages, tools, std::move(observer),
        [completion = std::move(completion), petName]
        (bool ok, LlmResponse response, QString error) mutable {
            const QString statsPetName = petName.isEmpty()
                ? QStringLiteral("AI_GLOBAL") : petName;
            StatisticManager::getInstance().recordLlmCall(
                statsPetName, ok, response.usage);
            if (completion) {
                completion(ok, std::move(response), std::move(error));
            }
        });
}
