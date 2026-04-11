//
// LLM 客户端抽象接口
//

#ifndef DESKTOP_PET_LLM_CLIENT_H
#define DESKTOP_PET_LLM_CLIENT_H

#include <QJsonArray>
#include <QList>
#include <QString>
#include <functional>

#include "ai_types.h"

using LlmCompletionHandler = std::function<void(bool success, LlmResponse response, QString errorMessage)>;

class LlmClient {
public:
    virtual ~LlmClient() = default;

    // 异步发送一次 chat completion 请求。
    // 回调保证在 Qt 事件循环中触发，不阻塞 UI 线程。
    virtual void sendChatCompletionAsync(const LlmConfig& config,
                                         const QList<ChatMessage>& messages,
                                         const QJsonArray& tools,
                                         LlmCompletionHandler callback) = 0;
};

#endif // DESKTOP_PET_LLM_CLIENT_H
