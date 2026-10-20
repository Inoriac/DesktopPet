//
// LLM 聊天服务实现
//

#include "llm_chat_service.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QUuid>
#include <atomic>
#include <memory>

#include "configLoader/config_manager.h"
#include "statistic_manager.h"

namespace {
void recordAttempt(const QString& id, const QString& petName, const LlmConfig& config,
                   const QElapsedTimer& elapsed, bool ok, const LlmResponse& response,
                   const QString& error) {
    ModelAttemptReport report;
    report.attemptId = id;
    report.petName = petName;
    report.provider = config.provider;
    report.model = response.model.isEmpty() ? config.model : response.model;
    report.success = ok;
    report.cancelled = error.startsWith(QStringLiteral("LLM_REQUEST_CANCELLED"));
    report.durationMs = elapsed.elapsed();
    report.completedAt = QDateTime::currentDateTimeUtc();
    report.usage = response.usage;
    StatisticManager::getInstance().recordLlmAttempt(report);
}
}

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
                                            const QString& petName,
                                            bool recordStatistics) {
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

    *retryInvoker = [client, cfg, messages, tools, callback, attempts, attemptIndex,
                     weakRetryInvoker, petName, recordStatistics]() mutable {
        const auto keepAlive = weakRetryInvoker.lock();
        if (!keepAlive) {
            return;
        }
        const QString attemptId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto terminal = std::make_shared<std::atomic_bool>(false);
        QElapsedTimer elapsed;
        elapsed.start();
        client->sendChatCompletionAsync(cfg, messages, tools,
            [callback, attempts, attemptIndex, keepAlive, petName, cfg, recordStatistics,
             attemptId, elapsed, terminal]
            (bool ok, LlmResponse response, QString error) mutable {
                if (terminal->exchange(true)) return;
                if (recordStatistics)
                    recordAttempt(attemptId, petName, cfg, elapsed, ok, response, error);
                if (ok) {
                    callback(true, std::move(response), {});
                    return;
                }

                *attemptIndex += 1;
                qWarning() << "[LlmChatService] Request failed, attempt" << *attemptIndex
                           << "/" << attempts << ", reason:" << error;

                if (*attemptIndex < attempts
                    && !error.startsWith(QStringLiteral("LLM_REQUEST_CANCELLED"))) {
                    (*keepAlive)();
                    return;
                }

                callback(false, std::move(response), error);
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
    const QString& petName,
    bool recordStatistics) {
    const auto selectedClient = clientForConfig(cfg);
    if (!selectedClient || !cfg.enabled) {
        auto handle = std::make_shared<llm_client_detail::LegacyRequestHandle>();
        handle->terminal.store(true);
        if (completion) {
            completion(false, {}, selectedClient
                ? QStringLiteral("LLM disabled by config (aiSettings.enabled=false)")
                : QStringLiteral("LLM_PROVIDER_UNSUPPORTED: no client registered for provider"));
        }
        return handle;
    }

    const QString attemptId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto terminal = std::make_shared<std::atomic_bool>(false);
    QElapsedTimer elapsed;
    elapsed.start();
    return selectedClient->sendChatCompletionStreamAsync(
        cfg, messages, tools, std::move(observer),
        [completion = std::move(completion), petName, cfg, recordStatistics,
         attemptId, terminal, elapsed]
        (bool ok, LlmResponse response, QString error) mutable {
            if (terminal->exchange(true)) return;
            if (recordStatistics)
                recordAttempt(attemptId, petName, cfg, elapsed, ok, response, error);
            if (completion) {
                completion(ok, std::move(response), std::move(error));
            }
        });
}
