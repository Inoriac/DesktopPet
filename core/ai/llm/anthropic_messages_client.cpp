#include "anthropic_messages_client.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#include <atomic>
#include <memory>
#include <utility>

#include "sse_event_parser.h"

namespace {

constexpr auto kProtocolError = "LLM_STREAM_PROTOCOL_ERROR";
constexpr auto kCancelledError = "LLM_REQUEST_CANCELLED";

struct AnthropicBlockState {
    QString type;
    QJsonObject block;
    QByteArray inputJson;
    bool stopped = false;
};

struct AnthropicRequestState;

class AnthropicRequestHandle final : public LlmRequestHandle {
public:
    explicit AnthropicRequestHandle(std::shared_ptr<AnthropicRequestState> state)
        : m_state(std::move(state)) {}

    void cancel() override;
    bool isCancelled() const override;

private:
    std::shared_ptr<AnthropicRequestState> m_state;
};

struct AnthropicRequestState : public std::enable_shared_from_this<AnthropicRequestState> {
    QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QPointer<QNetworkReply> reply;
    LlmStreamObserver observer;
    LlmCompletionHandler completion;
    LlmResponse response;
    QMap<int, AnthropicBlockState> blocks;
    std::unique_ptr<SseEventParser> parser;
    std::atomic_bool cancelled{false};
    std::atomic_bool terminal{false};
    bool sawMessageStop = false;
    bool sawMessageStart = false;
    bool streamingStagePublished = false;

    void publish(LlmStreamEventType type,
                 ChatActivityStage stage = ChatActivityStage::WaitingForModel,
                 const QString& text = {}) {
        if (!terminal.load() && !cancelled.load() && observer) {
            observer({type, requestId, stage, text});
        }
    }

    void complete(bool ok, LlmResponse value, QString error) {
        if (terminal.exchange(true)) {
            return;
        }
        auto callback = std::move(completion);
        observer = {};
        if (callback) {
            callback(ok, std::move(value), std::move(error));
        }
    }

    void failProtocol(const QString& detail) {
        complete(false, {}, QStringLiteral("%1: %2").arg(QString::fromLatin1(kProtocolError), detail));
        if (reply) {
            reply->abort();
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
        complete(false, {}, QStringLiteral("%1: request cancelled").arg(QString::fromLatin1(kCancelledError)));
    }
};

void AnthropicRequestHandle::cancel() {
    if (m_state) {
        m_state->cancel();
    }
}

bool AnthropicRequestHandle::isCancelled() const {
    return m_state && m_state->cancelled.load();
}

QUrl messagesUrl(const QString& baseUrl) {
    QUrl url(baseUrl.trimmed());
    QString path = url.path();
    while (path.size() > 1 && path.endsWith('/')) {
        path.chop(1);
    }
    if (path.endsWith(QStringLiteral("/messages"))) {
        // Already points at the Messages endpoint.
    } else if (path.endsWith(QStringLiteral("/v1"))) {
        path += QStringLiteral("/messages");
    } else {
        if (path == QStringLiteral("/")) {
            path.clear();
        }
        path += QStringLiteral("/v1/messages");
    }
    url.setPath(path);
    url.setQuery({});
    url.setFragment({});
    return url;
}

bool isAbsoluteHttpUrl(const QString& baseUrl) {
    const QUrl url(baseUrl.trimmed());
    const QString scheme = url.scheme().toLower();
    return url.isValid() && !url.isRelative() &&
           (scheme == QStringLiteral("http") || scheme == QStringLiteral("https")) &&
           !url.host().isEmpty();
}

bool appendMessage(QJsonArray& output, const QString& role, const QJsonArray& blocks) {
    if (role != QStringLiteral("user") && role != QStringLiteral("assistant")) {
        return false;
    }
    if (!output.isEmpty()) {
        QJsonObject last = output.last().toObject();
        if (last.value(QStringLiteral("role")).toString() == role) {
            QJsonArray content = last.value(QStringLiteral("content")).toArray();
            for (const QJsonValue& block : blocks) {
                content.append(block);
            }
            last[QStringLiteral("content")] = content;
            output.replace(output.size() - 1, last);
            return true;
        }
    }
    output.append(QJsonObject{{QStringLiteral("role"), role},
                              {QStringLiteral("content"), blocks}});
    return true;
}

bool convertMessages(const QList<ChatMessage>& messages,
                     QString& system,
                     QJsonArray& output,
                     QString& error) {
    QStringList systemParts;
    for (const ChatMessage& message : messages) {
        if (message.role == QStringLiteral("system")) {
            if (!message.content.isEmpty()) {
                systemParts.append(message.content);
            }
            continue;
        }

        QJsonArray blocks;
        QString role = message.role;
        if (role == QStringLiteral("assistant") && !message.transportBlocks.isEmpty()) {
            if (!appendMessage(output, role, message.transportBlocks)) {
                error = QStringLiteral("unsupported Anthropic message role");
                return false;
            }
            continue;
        }
        if (role == QStringLiteral("tool")) {
            if (message.toolCallId.trimmed().isEmpty()) {
                error = QStringLiteral("tool result is missing tool_call_id");
                return false;
            }
            role = QStringLiteral("user");
            blocks.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("tool_result")},
                {QStringLiteral("tool_use_id"), message.toolCallId},
                {QStringLiteral("content"), message.content}
            });
        } else {
            if (!message.content.isEmpty() || message.toolCalls.isEmpty()) {
                blocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                          {QStringLiteral("text"), message.content}});
            }
            if (role == QStringLiteral("assistant")) {
                for (const QJsonValue& value : message.toolCalls) {
                    const QJsonObject call = value.toObject();
                    const QJsonObject function = call.value(QStringLiteral("function")).toObject();
                    const QJsonDocument arguments = QJsonDocument::fromJson(
                        function.value(QStringLiteral("arguments")).toString().toUtf8());
                    if (!arguments.isObject()) {
                        error = QStringLiteral("assistant tool arguments must be a JSON object");
                        return false;
                    }
                    blocks.append(QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("tool_use")},
                        {QStringLiteral("id"), call.value(QStringLiteral("id")).toString()},
                        {QStringLiteral("name"), function.value(QStringLiteral("name")).toString()},
                        {QStringLiteral("input"), arguments.object()}
                    });
                }
            }
        }

        if (!appendMessage(output, role, blocks)) {
            error = QStringLiteral("unsupported Anthropic message role");
            return false;
        }
    }
    system = systemParts.join(QStringLiteral("\n\n"));
    return true;
}

bool convertTools(const QJsonArray& tools, QJsonArray& output, QString& error) {
    for (const QJsonValue& value : tools) {
        const QJsonObject tool = value.toObject();
        const QJsonObject function = tool.value(QStringLiteral("function")).toObject();
        const QString name = function.value(QStringLiteral("name")).toString().trimmed();
        if (tool.value(QStringLiteral("type")).toString() != QStringLiteral("function") ||
            name.isEmpty() || !function.value(QStringLiteral("parameters")).isObject()) {
            error = QStringLiteral("invalid function tool schema");
            return false;
        }
        QJsonObject converted{
            {QStringLiteral("name"), name},
            {QStringLiteral("input_schema"), function.value(QStringLiteral("parameters"))}
        };
        const QString description = function.value(QStringLiteral("description")).toString();
        if (!description.isEmpty()) {
            converted[QStringLiteral("description")] = description;
        }
        output.append(converted);
    }
    return true;
}

void mergeUsage(const QJsonObject& usage, LlmUsage& output) {
    if (usage.contains(QStringLiteral("input_tokens"))) {
        output.promptTokens = usage.value(QStringLiteral("input_tokens")).toVariant().toLongLong();
    }
    if (usage.contains(QStringLiteral("output_tokens"))) {
        output.completionTokens = usage.value(QStringLiteral("output_tokens")).toVariant().toLongLong();
    }
    if (usage.contains(QStringLiteral("cache_creation_input_tokens"))) {
        output.promptCacheMissTokens = usage.value(QStringLiteral("cache_creation_input_tokens")).toVariant().toLongLong();
    }
    if (usage.contains(QStringLiteral("cache_read_input_tokens"))) {
        output.promptCacheHitTokens = usage.value(QStringLiteral("cache_read_input_tokens")).toVariant().toLongLong();
        output.cachedTokens = output.promptCacheHitTokens;
    }
    output.totalTokens = output.promptTokens + output.completionTokens;
}

bool updateBlock(const QJsonObject& event, const std::shared_ptr<AnthropicRequestState>& state, QString& error) {
    const int index = event.value(QStringLiteral("index")).toInt(-1);
    if (index < 0 || !state->blocks.contains(index)) {
        error = QStringLiteral("content delta references an unknown block");
        return false;
    }
    AnthropicBlockState& block = state->blocks[index];
    if (block.stopped) {
        error = QStringLiteral("content delta follows block stop");
        return false;
    }
    const QJsonObject delta = event.value(QStringLiteral("delta")).toObject();
    const QString deltaType = delta.value(QStringLiteral("type")).toString();
    if (deltaType == QStringLiteral("text_delta") && block.type == QStringLiteral("text")) {
        const QString text = delta.value(QStringLiteral("text")).toString();
        block.block[QStringLiteral("text")] =
            block.block.value(QStringLiteral("text")).toString() + text;
        state->response.content += text;
        if (!text.isEmpty()) {
            if (!state->streamingStagePublished) {
                state->streamingStagePublished = true;
                state->publish(LlmStreamEventType::StageChanged, ChatActivityStage::StreamingText);
            }
            state->publish(LlmStreamEventType::TextDelta, ChatActivityStage::StreamingText, text);
        }
        return true;
    }
    if (deltaType == QStringLiteral("input_json_delta") && block.type == QStringLiteral("tool_use")) {
        block.inputJson += delta.value(QStringLiteral("partial_json")).toString().toUtf8();
        return true;
    }
    if (deltaType == QStringLiteral("thinking_delta") && block.type == QStringLiteral("thinking")) {
        block.block[QStringLiteral("thinking")] =
            block.block.value(QStringLiteral("thinking")).toString() +
            delta.value(QStringLiteral("thinking")).toString();
        return true;
    }
    if (deltaType == QStringLiteral("signature_delta") && block.type == QStringLiteral("thinking")) {
        block.block[QStringLiteral("signature")] =
            block.block.value(QStringLiteral("signature")).toString() +
            delta.value(QStringLiteral("signature")).toString();
        return true;
    }
    error = QStringLiteral("content delta type does not match its block");
    return false;
}

bool stopBlock(const QJsonObject& event, const std::shared_ptr<AnthropicRequestState>& state, QString& error) {
    const int index = event.value(QStringLiteral("index")).toInt(-1);
    if (index < 0 || !state->blocks.contains(index)) {
        error = QStringLiteral("content stop references an unknown block");
        return false;
    }
    AnthropicBlockState& block = state->blocks[index];
    if (block.stopped) {
        error = QStringLiteral("content block stopped more than once");
        return false;
    }
    block.stopped = true;

    if (block.type == QStringLiteral("tool_use")) {
        QJsonObject input = block.block.value(QStringLiteral("input")).toObject();
        if (!block.inputJson.isEmpty()) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(block.inputJson, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                error = QStringLiteral("tool input is not a complete JSON object");
                return false;
            }
            input = document.object();
        }
        block.block[QStringLiteral("input")] = input;
        LlmToolCall call;
        call.id = block.block.value(QStringLiteral("id")).toString();
        call.type = QStringLiteral("function");
        call.name = block.block.value(QStringLiteral("name")).toString();
        call.arguments = input;
        if (call.id.isEmpty() || call.name.isEmpty()) {
            error = QStringLiteral("tool block is missing id or name");
            return false;
        }
        state->response.toolCalls.append(std::move(call));
    }
    return true;
}

void consumeAnthropicEvent(const SseEvent& framed,
                           const std::shared_ptr<AnthropicRequestState>& state) {
    if (state->terminal.load()) {
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(framed.data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        state->failProtocol(QStringLiteral("event data is not a JSON object"));
        return;
    }
    const QJsonObject event = document.object();
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type.isEmpty() || (!framed.event.isEmpty() && framed.event != type)) {
        state->failProtocol(QStringLiteral("event type is missing or mismatched"));
        return;
    }

    if (type == QStringLiteral("ping")) {
        return;
    }
    if (type == QStringLiteral("error")) {
        state->failProtocol(QStringLiteral("provider returned an SSE error"));
        return;
    }
    if (type == QStringLiteral("message_start")) {
        if (state->sawMessageStart || state->sawMessageStop) {
            state->failProtocol(QStringLiteral("message_start appeared more than once or after message_stop"));
            return;
        }
        state->sawMessageStart = true;
        const QJsonObject message = event.value(QStringLiteral("message")).toObject();
        state->response.id = message.value(QStringLiteral("id")).toString();
        state->response.model = message.value(QStringLiteral("model")).toString();
        mergeUsage(message.value(QStringLiteral("usage")).toObject(), state->response.usage);
        return;
    }
    if (!state->sawMessageStart) {
        state->failProtocol(QStringLiteral("content event arrived before message_start"));
        return;
    }
    if (state->sawMessageStop) {
        state->failProtocol(QStringLiteral("content event arrived after message_stop"));
        return;
    }
    if (type == QStringLiteral("content_block_start")) {
        const int index = event.value(QStringLiteral("index")).toInt(-1);
        const QJsonObject contentBlock = event.value(QStringLiteral("content_block")).toObject();
        const QString blockType = contentBlock.value(QStringLiteral("type")).toString();
        if (index < 0 || state->blocks.contains(index) ||
            (blockType != QStringLiteral("text") &&
             blockType != QStringLiteral("thinking") &&
             blockType != QStringLiteral("tool_use"))) {
            state->failProtocol(QStringLiteral("invalid content block start"));
            return;
        }
        state->blocks.insert(index, AnthropicBlockState{blockType, contentBlock, {}, false});
        const QString initialText = blockType == QStringLiteral("text")
            ? contentBlock.value(QStringLiteral("text")).toString() : QString{};
        if (!initialText.isEmpty()) {
            state->response.content += initialText;
            if (!state->streamingStagePublished) {
                state->streamingStagePublished = true;
                state->publish(LlmStreamEventType::StageChanged, ChatActivityStage::StreamingText);
            }
            state->publish(LlmStreamEventType::TextDelta, ChatActivityStage::StreamingText, initialText);
        }
        return;
    }
    if (type == QStringLiteral("content_block_delta")) {
        QString error;
        if (!updateBlock(event, state, error)) {
            state->failProtocol(error);
        }
        return;
    }
    if (type == QStringLiteral("content_block_stop")) {
        QString error;
        if (!stopBlock(event, state, error)) {
            state->failProtocol(error);
        }
        return;
    }
    if (type == QStringLiteral("message_delta")) {
        state->response.finishReason = event.value(QStringLiteral("delta")).toObject()
                                           .value(QStringLiteral("stop_reason")).toString();
        mergeUsage(event.value(QStringLiteral("usage")).toObject(), state->response.usage);
        return;
    }
    if (type == QStringLiteral("message_stop")) {
        for (auto it = state->blocks.cbegin(); it != state->blocks.cend(); ++it) {
            if (!it->stopped) {
                state->failProtocol(QStringLiteral("message stopped with an open content block"));
                return;
            }
        }
        state->response.transportBlocks = {};
        for (auto it = state->blocks.cbegin(); it != state->blocks.cend(); ++it) {
            state->response.transportBlocks.append(it->block);
        }
        state->sawMessageStop = true;
        return;
    }
    state->failProtocol(QStringLiteral("unsupported Anthropic SSE event"));
}

bool validExtraHeaders(const QJsonObject& headers, QString& error) {
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        const QByteArray name = it.key().trimmed().toLatin1();
        const QByteArray lower = name.toLower();
        const QString value = it.value().toString();
        if (name.isEmpty() || !it.value().isString() || name.contains('\r') || name.contains('\n') ||
            value.contains('\r') || value.contains('\n')) {
            error = QStringLiteral("extraHeaders contains an invalid header");
            return false;
        }
        if (lower == "content-type" || lower == "x-api-key" || lower == "anthropic-version" ||
            lower == "content-length" || lower == "host") {
            error = QStringLiteral("extraHeaders cannot override required headers");
            return false;
        }
    }
    return true;
}

} // namespace

AnthropicMessagesClient::AnthropicMessagesClient() = default;

void AnthropicMessagesClient::sendChatCompletionAsync(const LlmConfig& config,
                                                      const QList<ChatMessage>& messages,
                                                      const QJsonArray& tools,
                                                      LlmCompletionHandler callback) {
    sendChatCompletionStreamAsync(config, messages, tools, {}, std::move(callback));
}

std::shared_ptr<LlmRequestHandle> AnthropicMessagesClient::sendChatCompletionStreamAsync(
    const LlmConfig& config,
    const QList<ChatMessage>& messages,
    const QJsonArray& tools,
    LlmStreamObserver observer,
    LlmCompletionHandler completion) {
    auto state = std::make_shared<AnthropicRequestState>();
    state->observer = std::move(observer);
    state->completion = std::move(completion);
    auto handle = std::make_shared<AnthropicRequestHandle>(state);

    auto failConfig = [&](const QString& detail) {
        state->complete(false, {}, QStringLiteral("LLM_CONFIG_INVALID: %1").arg(detail));
        return std::static_pointer_cast<LlmRequestHandle>(handle);
    };
    if (!state->completion) {
        return failConfig(QStringLiteral("completion callback is required"));
    }
    if (!config.enabled) {
        return failConfig(QStringLiteral("LLM is disabled"));
    }
    if (!isAbsoluteHttpUrl(config.baseUrl)) {
        return failConfig(QStringLiteral("baseUrl is empty or invalid"));
    }
    if (config.apiKey.trimmed().isEmpty()) {
        return failConfig(QStringLiteral("apiKey is empty"));
    }
    if (config.model.trimmed().isEmpty()) {
        return failConfig(QStringLiteral("model is empty"));
    }
    QString conversionError;
    if (!validExtraHeaders(config.extraHeaders, conversionError)) {
        return failConfig(conversionError);
    }

    QString system;
    QJsonArray convertedMessages;
    if (!convertMessages(messages, system, convertedMessages, conversionError)) {
        return failConfig(conversionError);
    }
    QJsonArray convertedTools;
    if (!convertTools(tools, convertedTools, conversionError)) {
        return failConfig(conversionError);
    }

    QJsonObject payload{
        {QStringLiteral("model"), config.model},
        {QStringLiteral("max_tokens"), config.maxTokens},
        {QStringLiteral("temperature"), config.temperature},
        {QStringLiteral("stream"), true},
        {QStringLiteral("messages"), convertedMessages}
    };
    if (!system.isEmpty()) {
        payload[QStringLiteral("system")] = system;
    }
    if (!convertedTools.isEmpty()) {
        payload[QStringLiteral("tools")] = convertedTools;
    }
    for (auto it = config.extraParams.begin(); it != config.extraParams.end(); ++it) {
        if (it.key() != QStringLiteral("stream")) {
            payload[it.key()] = it.value();
        }
    }

    QNetworkRequest request(messagesUrl(config.baseUrl));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("x-api-key", config.apiKey.toUtf8());
    request.setRawHeader("anthropic-version",
                         config.anthropicVersion.trimmed().isEmpty()
                             ? QByteArray("2023-06-01")
                             : config.anthropicVersion.trimmed().toUtf8());
    request.setTransferTimeout(config.timeoutMs);
    for (auto it = config.extraHeaders.begin(); it != config.extraHeaders.end(); ++it) {
        request.setRawHeader(it.key().toLatin1(), it.value().toString().toUtf8());
    }

    state->parser = std::make_unique<SseEventParser>(
        [weakState = std::weak_ptr<AnthropicRequestState>(state)](const SseEvent& event) {
            if (const auto locked = weakState.lock()) {
                consumeAnthropicEvent(event, locked);
            }
        });
    QNetworkReply* reply = m_network.post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    state->reply = reply;
    state->publish(LlmStreamEventType::Started);

    QObject::connect(reply, &QNetworkReply::readyRead, reply, [state]() {
        if (state->terminal.load()) {
            if (state->reply && state->reply->isOpen()) {
                state->reply->readAll();
            }
            return;
        }
        QString error;
        if (!state->parser->feed(state->reply->readAll(), &error)) {
            state->failProtocol(error);
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, [state, reply]() {
        const QByteArray finalBytes = reply->isOpen() ? reply->readAll() : QByteArray{};
        if (!state->terminal.load() && !finalBytes.isEmpty()) {
            QString error;
            if (!state->parser->feed(finalBytes, &error)) {
                state->failProtocol(error);
            }
        }

        if (!state->terminal.load()) {
            if (state->cancelled.load()) {
                state->complete(false, {}, QStringLiteral("%1: request cancelled").arg(QString::fromLatin1(kCancelledError)));
            } else {
                const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (reply->error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300) {
                    state->complete(false, {}, QStringLiteral("LLM network error (HTTP %1, code %2)")
                                                   .arg(httpStatus)
                                                   .arg(static_cast<int>(reply->error())));
                } else {
                    QString error;
                    if (!state->parser->finish(&error)) {
                        state->failProtocol(error);
                    } else if (!state->sawMessageStop) {
                        state->failProtocol(QStringLiteral("stream ended without message_stop"));
                    } else {
                        state->complete(true, std::move(state->response), {});
                    }
                }
            }
        }
        reply->deleteLater();
    });

    return handle;
}
