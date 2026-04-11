//
// OpenAI Compatible 客户端
// 支持 OpenAI 兼容接口，包括 SiliconFlow 等网关
//

#ifndef DESKTOP_PET_OPENAI_COMPATIBLE_CLIENT_H
#define DESKTOP_PET_OPENAI_COMPATIBLE_CLIENT_H

#include <QNetworkAccessManager>

#include "llm_client.h"

class OpenAICompatibleClient : public LlmClient {
public:
    OpenAICompatibleClient();
    ~OpenAICompatibleClient() override = default;

    void sendChatCompletionAsync(const LlmConfig& config,
                                 const QList<ChatMessage>& messages,
                                 const QJsonArray& tools,
                                 LlmCompletionHandler callback) override;

private:
    QNetworkAccessManager m_network;

    static QJsonArray buildMessagesArray(const QList<ChatMessage>& messages);
    static QUrl buildCompletionsUrl(const QString& baseUrl);
    static bool parseResponseBody(const QByteArray& body,
                                  LlmResponse& outResponse,
                                  QString& errorMessage);
};

#endif // DESKTOP_PET_OPENAI_COMPATIBLE_CLIENT_H
