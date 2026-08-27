#ifndef DESKTOP_PET_ANTHROPIC_MESSAGES_CLIENT_H
#define DESKTOP_PET_ANTHROPIC_MESSAGES_CLIENT_H

#include <QNetworkAccessManager>

#include "llm_client.h"

class AnthropicMessagesClient final : public LlmClient {
public:
    AnthropicMessagesClient();
    ~AnthropicMessagesClient() override = default;

    void sendChatCompletionAsync(const LlmConfig& config,
                                 const QList<ChatMessage>& messages,
                                 const QJsonArray& tools,
                                 LlmCompletionHandler callback) override;

    std::shared_ptr<LlmRequestHandle> sendChatCompletionStreamAsync(
        const LlmConfig& config,
        const QList<ChatMessage>& messages,
        const QJsonArray& tools,
        LlmStreamObserver observer,
        LlmCompletionHandler completion) override;

private:
    QNetworkAccessManager m_network;
};

#endif // DESKTOP_PET_ANTHROPIC_MESSAGES_CLIENT_H
