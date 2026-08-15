//
// LLM 聊天服务实现
//

#include "llm_chat_service.h"

#include <QDebug>
#include <memory>

#include "configLoader/config_manager.h"
#include "statistic_manager.h"

LlmChatService::LlmChatService(std::shared_ptr<LlmClient> client)
    : m_client(std::move(client)) {
    if (!m_client) {
        m_client = std::make_shared<OpenAICompatibleClient>();
    }
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
    if (!m_client) {
        callback(false, {}, "LLM client is not initialized");
        return;
    }

    if (!cfg.enabled) {
        callback(false, {}, "LLM disabled by config (aiSettings.enabled=false)");
        return;
    }

    const int attempts = qMax(1, cfg.retryCount + 1);
    auto attemptIndex = std::make_shared<int>(0);
    auto client = m_client;
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
                    StatisticManager::getInstance().recordLlmUsage(statsPetName, response.usage);
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

                callback(false, {}, error);
            });
    };

    (*retryInvoker)();
}
