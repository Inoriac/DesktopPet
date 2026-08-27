//
// OpenAI Compatible 客户端实现
//

#include "openai_compatible_client.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUuid>
#include <QDebug>
#include <atomic>
#include <memory>
#include <utility>

namespace {

struct OpenAiStreamState;

class OpenAiRequestHandle final : public LlmRequestHandle {
public:
    explicit OpenAiRequestHandle(std::shared_ptr<OpenAiStreamState> state)
        : m_state(std::move(state)) {}

    void cancel() override;
    bool isCancelled() const override;

private:
    std::shared_ptr<OpenAiStreamState> m_state;
};

struct OpenAiStreamState {
    QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QPointer<QNetworkReply> reply;
    LlmStreamObserver observer;
    LlmCompletionHandler completion;
    std::atomic_bool cancelled{false};
    std::atomic_bool terminal{false};

    void complete(bool ok, LlmResponse response, QString error) {
        if (terminal.exchange(true)) {
            return;
        }
        auto callback = std::move(completion);
        observer = {};
        if (callback) {
            callback(ok, std::move(response), std::move(error));
        }
    }

    void cancel() {
        if (terminal.load()) {
            return;
        }
        cancelled.store(true);
        if (reply) {
            reply->abort();
        }
        complete(false, {}, QStringLiteral("LLM_REQUEST_CANCELLED: request cancelled"));
    }
};

void OpenAiRequestHandle::cancel() {
    if (m_state) {
        m_state->cancel();
    }
}

bool OpenAiRequestHandle::isCancelled() const {
    return m_state && m_state->cancelled.load();
}

QString redactSecret(QString message, const QString& secret) {
    if (!secret.isEmpty()) {
        message.replace(secret, QStringLiteral("[REDACTED]"), Qt::CaseSensitive);
    }
    return message;
}

} // namespace

OpenAICompatibleClient::OpenAICompatibleClient() = default;

std::shared_ptr<LlmRequestHandle> OpenAICompatibleClient::sendChatCompletionStreamAsync(
    const LlmConfig& config,
    const QList<ChatMessage>& messages,
    const QJsonArray& tools,
    LlmStreamObserver observer,
    LlmCompletionHandler completion) {
    auto state = std::make_shared<OpenAiStreamState>();
    state->observer = std::move(observer);
    state->completion = std::move(completion);
    auto handle = std::make_shared<OpenAiRequestHandle>(state);

    if (state->observer) {
        state->observer({LlmStreamEventType::Started, state->requestId,
                         ChatActivityStage::WaitingForModel, {}});
    }
    state->reply = startChatCompletionRequest(
        config, messages, tools,
        [state](bool ok, LlmResponse response, QString error) mutable {
            if (state->terminal.load()) {
                return;
            }
            if (state->cancelled.load()) {
                state->complete(false, {}, QStringLiteral("LLM_REQUEST_CANCELLED: request cancelled"));
                return;
            }
            if (ok && state->observer && !response.content.isEmpty()) {
                state->observer({LlmStreamEventType::StageChanged, state->requestId,
                                 ChatActivityStage::StreamingText, {}});
                state->observer({LlmStreamEventType::TextDelta, state->requestId,
                                 ChatActivityStage::StreamingText, response.content});
            }
            state->complete(ok, std::move(response), std::move(error));
        });
    return handle;
}

// 发送异步请求，回调在 Qt 事件循环中触发，统一返回成功结果或错误
void OpenAICompatibleClient::sendChatCompletionAsync(const LlmConfig& config,
                                                     const QList<ChatMessage>& messages,
                                                     const QJsonArray& tools,
                                                     LlmCompletionHandler callback) {
    startChatCompletionRequest(config, messages, tools, std::move(callback));
}

QNetworkReply* OpenAICompatibleClient::startChatCompletionRequest(
    const LlmConfig& config,
    const QList<ChatMessage>& messages,
    const QJsonArray& tools,
    LlmCompletionHandler callback) {
    const auto fail = [&callback](const QString& error) {
        if (callback) {
            callback(false, {}, error);
        }
    };
    if (!config.enabled) {
        fail(QStringLiteral("LLM is disabled in config"));
        return nullptr;
    }

    if (config.baseUrl.trimmed().isEmpty()) {
        fail(QStringLiteral("LLM baseUrl is empty"));
        return nullptr;
    }

    if (config.apiKey.trimmed().isEmpty()) {
        fail(QStringLiteral("LLM apiKey is empty"));
        return nullptr;
    }

    if (config.model.trimmed().isEmpty()) {
        fail(QStringLiteral("LLM model is empty"));
        return nullptr;
    }

    QNetworkRequest request(buildCompletionsUrl(config.baseUrl));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(config.apiKey).toUtf8());
    request.setTransferTimeout(config.timeoutMs);

    QJsonObject payload;
    payload["model"] = config.model;
    payload["messages"] = buildMessagesArray(messages);
    payload["temperature"] = config.temperature;
    payload["max_tokens"] = config.maxTokens;

    if (!tools.isEmpty()) {
        payload["tools"] = tools;
        payload["tool_choice"] = "auto";
    }

    // 允许通过配置注入额外参数，便于适配不同网关。
    for (auto it = config.extraParams.begin(); it != config.extraParams.end(); ++it) {
        payload[it.key()] = it.value();
    }

    // 当前客户端只支持非流式 JSON 响应；若用户配置了 stream=true，则自动降级。
    if (payload.contains("stream") && payload.value("stream").toBool(false)) {
        qWarning() << "[OpenAICompatibleClient] stream=true is not supported yet, forcing stream=false";
        payload["stream"] = false;
    }

    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = m_network.post(request, body);

    QObject::connect(reply, &QNetworkReply::finished, reply,
                     [reply, apiKey = config.apiKey, callback = std::move(callback)]() mutable {
        const QByteArray responseBytes = reply->isOpen() ? reply->readAll() : QByteArray{};
        const QNetworkReply::NetworkError networkError = reply->error();

        if (networkError != QNetworkReply::NoError) {
            const QString errorMessage = redactSecret(
                QStringLiteral("Network error (%1): %2")
                    .arg(static_cast<int>(networkError))
                    .arg(reply->errorString()),
                apiKey);
            if (callback) {
                callback(false, {}, errorMessage);
            }
            reply->deleteLater();
            return;
        }

        LlmResponse response;
        QString parseError;
        const bool ok = parseResponseBody(responseBytes, response, parseError);
        if (callback) {
            callback(ok, std::move(response), redactSecret(parseError, apiKey));
        }
        reply->deleteLater();
    });
    return reply;
}

QJsonArray OpenAICompatibleClient::buildMessagesArray(const QList<ChatMessage>& messages) {
    QJsonArray arr;
    for (const ChatMessage& msg : messages) {
        QJsonObject item;
        item["role"] = msg.role;
        item["content"] = msg.content;

        if (!msg.name.isEmpty()) {
            item["name"] = msg.name;
        }

        if (!msg.toolCalls.isEmpty()) {
            item["tool_calls"] = msg.toolCalls;
        }

        // tool 角色消息需要 tool_call_id。
        if (!msg.toolCallId.isEmpty()) {
            item["tool_call_id"] = msg.toolCallId;
        }

        arr.append(item);
    }
    return arr;
}

QUrl OpenAICompatibleClient::buildCompletionsUrl(const QString& baseUrl) {
    QString normalized = baseUrl.trimmed();

    // 兼容两种传参：
    // 1) 只给 /v1
    // 2) 直接给 /v1/chat/completions
    if (normalized.endsWith('/')) {
        normalized.chop(1);
    }

    if (normalized.endsWith("/chat/completions")) {
        return QUrl(normalized);
    }

    return QUrl(normalized + "/chat/completions");
}

bool OpenAICompatibleClient::parseResponseBody(const QByteArray& body,
                                               LlmResponse& outResponse,
                                               QString& errorMessage) {
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        errorMessage = "Invalid JSON response from LLM";
        return false;
    }

    const QJsonObject root = doc.object();

    outResponse.id = root.value("id").toString();
    outResponse.model = root.value("model").toString();
    outResponse.created = root.value("created").toVariant().toLongLong();

    const QJsonObject usageObj = root.value("usage").toObject();
    outResponse.usage.promptTokens = usageObj.value("prompt_tokens").toVariant().toLongLong();
    outResponse.usage.completionTokens = usageObj.value("completion_tokens").toVariant().toLongLong();
    outResponse.usage.totalTokens = usageObj.value("total_tokens").toVariant().toLongLong();
    outResponse.usage.promptCacheHitTokens = usageObj.value("prompt_cache_hit_tokens").toVariant().toLongLong();
    outResponse.usage.promptCacheMissTokens = usageObj.value("prompt_cache_miss_tokens").toVariant().toLongLong();

    const QJsonObject completionDetails = usageObj.value("completion_tokens_details").toObject();
    outResponse.usage.reasoningTokens = completionDetails.value("reasoning_tokens").toVariant().toLongLong();

    const QJsonObject promptDetails = usageObj.value("prompt_tokens_details").toObject();
    outResponse.usage.cachedTokens = promptDetails.value("cached_tokens").toVariant().toLongLong();

    if (root.contains("error")) {
        const QJsonObject errObj = root.value("error").toObject();
        const QString message = errObj.value("message").toString("Unknown LLM error");
        errorMessage = QString("LLM returned error: %1").arg(message);
        return false;
    }

    const QJsonArray choices = root.value("choices").toArray();
    if (choices.isEmpty()) {
        errorMessage = "LLM response has no choices";
        return false;
    }

    const QJsonObject firstChoice = choices.first().toObject();
    const QJsonObject messageObj = firstChoice.value("message").toObject();

    outResponse.content = messageObj.value("content").toString();
    outResponse.reasoningContent = messageObj.value("reasoning_content").toString();
    outResponse.finishReason = firstChoice.value("finish_reason").toString();

    const QJsonArray toolCalls = messageObj.value("tool_calls").toArray();
    for (const auto& item : toolCalls) {
        const QJsonObject callObj = item.toObject();
        const QJsonObject functionObj = callObj.value("function").toObject();

        LlmToolCall call;
        call.id = callObj.value("id").toString();
        call.type = callObj.value("type").toString("function");
        call.name = functionObj.value("name").toString();

        const QString argumentsRaw = functionObj.value("arguments").toString("{}");
        const QJsonDocument argsDoc = QJsonDocument::fromJson(argumentsRaw.toUtf8());
        if (argsDoc.isObject()) {
            call.arguments = argsDoc.object();
        } else {
            // 某些模型会返回非严格 JSON；先兜底保留原文。
            call.arguments["_raw"] = argumentsRaw;
        }

        outResponse.toolCalls.append(call);
    }

    return true;
}
