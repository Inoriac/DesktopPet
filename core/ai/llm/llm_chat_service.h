//
// LLM 聊天服务
// 封装配置读取 + 重试逻辑，供上层 AIBrain 直接调用
//

#ifndef DESKTOP_PET_LLM_CHAT_SERVICE_H
#define DESKTOP_PET_LLM_CHAT_SERVICE_H

#include <QJsonArray>
#include <QList>
#include <QString>
#include <memory>

#include "ai_types.h"
#include "anthropic_messages_client.h"
#include "openai_compatible_client.h"

class LlmChatService {
public:
    explicit LlmChatService(std::shared_ptr<LlmClient> client = nullptr);

    // 从 ConfigManager 读取配置后发起异步请求。
    void requestAsync(const QList<ChatMessage>& messages,
                      const QJsonArray& tools,
                      LlmCompletionHandler callback,
                      const QString& petName = "AI_GLOBAL");

    // 显式传入配置，便于测试与高级场景。
    void requestAsyncWithConfig(const LlmConfig& cfg,
                                const QList<ChatMessage>& messages,
                                const QJsonArray& tools,
                                LlmCompletionHandler callback,
                                const QString& petName = "AI_GLOBAL");

    std::shared_ptr<LlmRequestHandle> requestStreamAsync(
        const QList<ChatMessage>& messages,
        const QJsonArray& tools,
        LlmStreamObserver observer,
        LlmCompletionHandler completion,
        const QString& petName = "AI_GLOBAL");

    std::shared_ptr<LlmRequestHandle> requestStreamAsyncWithConfig(
        const LlmConfig& cfg,
        const QList<ChatMessage>& messages,
        const QJsonArray& tools,
        LlmStreamObserver observer,
        LlmCompletionHandler completion,
        const QString& petName = "AI_GLOBAL");

private:
    std::shared_ptr<LlmClient> clientForConfig(const LlmConfig& cfg) const;

    std::shared_ptr<LlmClient> m_clientOverride;
    std::shared_ptr<LlmClient> m_openAiClient;
    std::shared_ptr<LlmClient> m_anthropicClient;
};

#endif // DESKTOP_PET_LLM_CHAT_SERVICE_H
