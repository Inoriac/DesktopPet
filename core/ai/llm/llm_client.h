//
// LLM 客户端抽象接口
//

#ifndef DESKTOP_PET_LLM_CLIENT_H
#define DESKTOP_PET_LLM_CLIENT_H

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QUuid>
#include <atomic>
#include <functional>
#include <memory>

#include "ai_types.h"
#include "llm_stream_types.h"

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

    virtual std::shared_ptr<LlmRequestHandle> sendChatCompletionStreamAsync(
        const LlmConfig& config,
        const QList<ChatMessage>& messages,
        const QJsonArray& tools,
        LlmStreamObserver observer,
        LlmCompletionHandler completion);
};

namespace llm_client_detail {
class LegacyRequestHandle final : public LlmRequestHandle {
public:
    void cancel() override {
        if (!terminal.load()) {
            cancelled.store(true);
        }
    }

    bool isCancelled() const override {
        return cancelled.load();
    }

    std::atomic_bool cancelled{false};
    std::atomic_bool terminal{false};
};
} // namespace llm_client_detail

inline std::shared_ptr<LlmRequestHandle> LlmClient::sendChatCompletionStreamAsync(
    const LlmConfig& config,
    const QList<ChatMessage>& messages,
    const QJsonArray& tools,
    LlmStreamObserver observer,
    LlmCompletionHandler completion) {
    auto handle = std::make_shared<llm_client_detail::LegacyRequestHandle>();
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (observer) {
        observer({LlmStreamEventType::Started, requestId,
                  ChatActivityStage::WaitingForModel, {}});
    }
    sendChatCompletionAsync(config, messages, tools,
        [handle, observer = std::move(observer), completion = std::move(completion), requestId]
        (bool ok, LlmResponse response, QString error) mutable {
            if (handle->terminal.exchange(true)) {
                return;
            }
            if (handle->cancelled.load()) {
                if (completion) {
                    completion(false, {}, QStringLiteral("LLM_REQUEST_CANCELLED: request cancelled"));
                }
                return;
            }
            if (ok && observer && !response.content.isEmpty()) {
                observer({LlmStreamEventType::StageChanged, requestId,
                          ChatActivityStage::StreamingText, {}});
                observer({LlmStreamEventType::TextDelta, requestId,
                          ChatActivityStage::StreamingText, response.content});
            }
            if (completion) {
                completion(ok, std::move(response), std::move(error));
            }
        });
    return handle;
}

#endif // DESKTOP_PET_LLM_CLIENT_H
