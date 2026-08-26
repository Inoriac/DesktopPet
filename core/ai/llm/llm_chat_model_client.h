#ifndef DESKTOP_PET_LLM_CHAT_MODEL_CLIENT_H
#define DESKTOP_PET_LLM_CHAT_MODEL_CLIENT_H

#include "ai/model/model_router.h"

class LlmChatService;

class LlmChatModelClient final : public ModelCompletionClient {
public:
    explicit LlmChatModelClient(LlmChatService* service);

    void completeOnce(const ModelRouteConfig& route,
                      const QList<ChatMessage>& messages,
                      const QJsonArray& tools,
                      LlmCompletionHandler callback,
                      const QString& petName) override;

private:
    LlmChatService* m_service = nullptr;
};

#endif // DESKTOP_PET_LLM_CHAT_MODEL_CLIENT_H
