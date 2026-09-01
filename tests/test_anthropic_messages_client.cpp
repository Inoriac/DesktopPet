#include <QCoreApplication>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

#include "llm/anthropic_messages_client.h"
#include "llm/openai_compatible_client.h"
#include "llm/sse_event_parser.h"

namespace {

QByteArray sseEvent(const QByteArray& type, const QJsonObject& payload) {
    return "event: " + type + "\n" +
           "data: " + QJsonDocument(payload).toJson(QJsonDocument::Compact) + "\n\n";
}

QByteArray textResponse(const QString& text = QStringLiteral("hello")) {
    QByteArray body;
    body += sseEvent("message_start", {
        {"type", "message_start"},
        {"message", QJsonObject{
            {"id", "msg_test"},
            {"model", "claude-test"},
            {"usage", QJsonObject{
                {"input_tokens", 7},
                {"cache_creation_input_tokens", 2},
                {"cache_read_input_tokens", 3}
            }}
        }}
    });
    body += sseEvent("content_block_start", {
        {"type", "content_block_start"},
        {"index", 0},
        {"content_block", QJsonObject{{"type", "text"}, {"text", ""}}}
    });
    const qsizetype split = qMax<qsizetype>(1, text.size() / 2);
    body += sseEvent("content_block_delta", {
        {"type", "content_block_delta"},
        {"index", 0},
        {"delta", QJsonObject{{"type", "text_delta"}, {"text", text.left(split)}}}
    });
    body += sseEvent("content_block_delta", {
        {"type", "content_block_delta"},
        {"index", 0},
        {"delta", QJsonObject{{"type", "text_delta"}, {"text", text.mid(split)}}}
    });
    body += sseEvent("content_block_stop", {{"type", "content_block_stop"}, {"index", 0}});
    body += sseEvent("message_delta", {
        {"type", "message_delta"},
        {"delta", QJsonObject{{"stop_reason", "end_turn"}}},
        {"usage", QJsonObject{{"output_tokens", 5}}}
    });
    body += sseEvent("message_stop", {{"type", "message_stop"}});
    return body;
}

class LocalHttpServer {
public:
    struct Response {
        int status = 200;
        QByteArray contentType = "text/event-stream";
        QByteArray body;
        bool holdOpen = false;
    };

    LocalHttpServer() {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                m_buffers.insert(socket, {});
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    QByteArray& buffer = m_buffers[socket];
                    buffer += socket->readAll();
                    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
                    if (headerEnd < 0) {
                        return;
                    }

                    const QByteArray header = buffer.left(headerEnd);
                    qsizetype contentLength = 0;
                    for (const QByteArray& line : header.split('\n')) {
                        const QByteArray trimmed = line.trimmed();
                        if (trimmed.toLower().startsWith("content-length:")) {
                            contentLength = trimmed.mid(sizeof("content-length:") - 1).trimmed().toLongLong();
                        }
                    }
                    const qsizetype bodyStart = headerEnd + 4;
                    if (buffer.size() - bodyStart < contentLength) {
                        return;
                    }

                    const QList<QByteArray> requestLine = header.split('\n').value(0).trimmed().split(' ');
                    requestTargets.append(requestLine.value(1));
                    requestHeaders.append(header);
                    requestBodies.append(buffer.mid(bodyStart, contentLength));
                    m_buffers.remove(socket);

                    const Response response = m_responses.isEmpty() ? Response{} : m_responses.takeFirst();
                    QByteArray responseHeader = "HTTP/1.1 " + QByteArray::number(response.status) +
                                                (response.status == 200 ? " OK\r\n" : " Error\r\n");
                    responseHeader += "Content-Type: " + response.contentType + "\r\n";
                    if (!response.holdOpen) {
                        responseHeader += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
                    }
                    responseHeader += "Connection: close\r\n\r\n";
                    socket->write(responseHeader);
                    socket->write(response.body);
                    socket->flush();
                    if (!response.holdOpen) {
                        socket->disconnectFromHost();
                    }
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket]() {
                    ++clientDisconnects;
                    m_buffers.remove(socket);
                    socket->deleteLater();
                });
            }
        });
        const bool listening = m_server.listen(QHostAddress::LocalHost, 0);
        Q_ASSERT(listening);
    }

    void enqueue(Response response) { m_responses.append(std::move(response)); }

    QString rootUrl() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }

    QList<QByteArray> requestTargets;
    QList<QByteArray> requestHeaders;
    QList<QByteArray> requestBodies;
    int clientDisconnects = 0;

private:
    QTcpServer m_server;
    QList<Response> m_responses;
    QHash<QTcpSocket*, QByteArray> m_buffers;
};

LlmConfig anthropicConfig(const LocalHttpServer& server) {
    LlmConfig config;
    config.enabled = true;
    config.provider = QStringLiteral("anthropic-messages");
    config.baseUrl = server.rootUrl();
    config.apiKey = QStringLiteral("test-secret");
    config.model = QStringLiteral("claude-test");
    config.maxTokens = 128;
    config.temperature = 0.25;
    config.timeoutMs = 2000;
    return config;
}

class DeferredLegacyClient final : public LlmClient {
public:
    void sendChatCompletionAsync(const LlmConfig&,
                                 const QList<ChatMessage>&,
                                 const QJsonArray&,
                                 LlmCompletionHandler callback) override {
        pending = std::move(callback);
    }

    void finish() {
        if (pending) {
            LlmResponse response;
            response.content = QStringLiteral("late");
            auto callback = std::move(pending);
            callback(true, std::move(response), {});
        }
    }

private:
    LlmCompletionHandler pending;
};

} // namespace

class TestAnthropicMessagesClient : public QObject {
    Q_OBJECT

private slots:
    void feed_whenEventIsSplitAcrossChunks_shouldPublishOneCompleteEvent();
    void feed_whenDataHasMultipleLinesAndCrLf_shouldJoinDataInOrder();
    void feed_whenMalformedFieldIsReceived_shouldReturnSafeFramingError();
    void feed_whenUnknownFieldIsReceived_shouldIgnoreItAndContinue();
    void finish_whenFinalEventHasNoBlankLine_shouldPublishCompleteEvent();
    void finish_whenFinalLineIsIncomplete_shouldReturnFramingError();
    void sendChatCompletionStreamAsync_whenTextStreamCompletes_shouldPublishOrderedDeltasAndUsage();
    void buildMessages_whenCanonicalImageBlockUsesAnthropic_shouldBuildBase64ImageSource();
    void buildMessages_whenCanonicalImageBlockUsesOpenAiCompatible_shouldBuildImageUrlDataUri();
    void sendChatCompletionStreamAsync_whenAnthropicTextBlockIsInvalid_shouldFailBeforeNetwork();
    void sendChatCompletionStreamAsync_whenOpenAiCanonicalBlockIsInvalid_shouldFailBeforeNetwork();
    void sendChatCompletionStreamAsync_whenBaseUrlHasRootV1OrMessages_shouldNormalizeOneEndpoint();
    void sendChatCompletionStreamAsync_whenToolInputArrivesInFragments_shouldCompleteOneValidatedToolCall();
    void sendChatCompletionStreamAsync_whenThinkingAndSignatureArrive_shouldKeepThemOutOfVisibleEvents();
    void sendChatCompletionStreamAsync_whenContinuationHasTransportBlocks_shouldReplayExactAssistantBlocks();
    void sendChatCompletionStreamAsync_whenMessageStopIsMissing_shouldFailExactlyOnce();
    void sendChatCompletionStreamAsync_whenMessageStopArrivesBeforeMessageStart_shouldFailExactlyOnce();
    void sendChatCompletionStreamAsync_whenContentArrivesAfterMessageStop_shouldFailExactlyOnce();
    void sendChatCompletionStreamAsync_whenProviderReturnsHttpError_shouldPreserveSafeDetail();
    void sendChatCompletionStreamAsync_whenBaseUrlIsNotAbsoluteHttp_shouldRejectWithoutRequest();
    void cancel_whenReplyIsActive_shouldAbortAndCompleteExactlyOnce();
    void cancel_whenReplyAlreadyFinished_shouldBeIdempotent();
    void sendChatCompletionStreamAsync_whenLegacyCompletionSucceeds_shouldPublishOneDeltaThenComplete();
    void cancel_whenOpenAiReplyIsActive_shouldAbortAndCompleteExactlyOnce();
    void sendChatCompletionStreamAsync_whenOpenAiErrorContainsApiKey_shouldNotExposeIt();
    void cancel_whenLegacyAdapterFinishes_shouldRemainObservableWithEmptyCompletion();
};

void TestAnthropicMessagesClient::feed_whenEventIsSplitAcrossChunks_shouldPublishOneCompleteEvent() {
    QList<SseEvent> events;
    SseEventParser parser([&](const SseEvent& event) { events.append(event); });
    QString error;
    QVERIFY(parser.feed("event: content_block_delta\n", &error));
    QCOMPARE(events.size(), 0);
    QVERIFY(parser.feed("data: {\"delta\":1}\n", &error));
    QCOMPARE(events.size(), 0);
    QVERIFY(parser.feed("\n", &error));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().event, QStringLiteral("content_block_delta"));
    QCOMPARE(events.first().data, QByteArray("{\"delta\":1}"));
}

void TestAnthropicMessagesClient::feed_whenDataHasMultipleLinesAndCrLf_shouldJoinDataInOrder() {
    QList<SseEvent> events;
    SseEventParser parser([&](const SseEvent& event) { events.append(event); });
    QString error;
    QVERIFY(parser.feed("event: sample\r\ndata: first\r\ndata: second\r\n\r\n", &error));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().data, QByteArray("first\nsecond"));
}

void TestAnthropicMessagesClient::feed_whenMalformedFieldIsReceived_shouldReturnSafeFramingError() {
    SseEventParser parser([](const SseEvent&) {});
    QString error;
    QByteArray invalid("data: safe-prefix");
    invalid.append('\0');
    invalid.append("private-suffix\n\n");
    QVERIFY(!parser.feed(invalid, &error));
    QVERIFY(error.contains(QStringLiteral("framing"), Qt::CaseInsensitive));
    QVERIFY(!error.contains(QStringLiteral("private-suffix")));
}

void TestAnthropicMessagesClient::feed_whenUnknownFieldIsReceived_shouldIgnoreItAndContinue() {
    QList<SseEvent> events;
    SseEventParser parser([&](const SseEvent& event) { events.append(event); });
    QString error;
    QVERIFY(parser.feed("unknown-without-colon\nunknown-field: ignored\nevent: message_stop\ndata: {}\n\n", &error));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().event, QStringLiteral("message_stop"));
    QCOMPARE(events.first().data, QByteArray("{}"));
}

void TestAnthropicMessagesClient::finish_whenFinalEventHasNoBlankLine_shouldPublishCompleteEvent() {
    QList<SseEvent> events;
    SseEventParser parser([&](const SseEvent& event) { events.append(event); });
    QString error;
    QVERIFY(parser.feed("event: message_stop\ndata: {}\n", &error));
    QCOMPARE(events.size(), 0);
    QVERIFY(parser.finish(&error));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().event, QStringLiteral("message_stop"));
}

void TestAnthropicMessagesClient::finish_whenFinalLineIsIncomplete_shouldReturnFramingError() {
    SseEventParser parser([](const SseEvent&) {});
    QString error;
    QVERIFY(parser.feed("event: message_stop\ndata: {}", &error));
    QVERIFY(!parser.finish(&error));
    QVERIFY(error.contains(QStringLiteral("framing"), Qt::CaseInsensitive));
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenTextStreamCompletes_shouldPublishOrderedDeltasAndUsage() {
    LocalHttpServer server;
    server.enqueue({200, "text/event-stream", textResponse(QStringLiteral("hello")), false});
    AnthropicMessagesClient client;
    QList<LlmStreamEvent> events;
    int completions = 0;
    bool success = false;
    LlmResponse response;
    QString error;

    QJsonArray tools{QJsonObject{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "get_weather"},
            {"description", "Get weather"},
            {"parameters", QJsonObject{{"type", "object"}}}
        }}
    }};
    const auto handle = client.sendChatCompletionStreamAsync(
        anthropicConfig(server), {{"system", "Be concise", {}, {}, {}}}, tools,
        [&](const LlmStreamEvent& event) { events.append(event); },
        [&](bool ok, LlmResponse value, QString message) {
            ++completions;
            success = ok;
            response = std::move(value);
            error = std::move(message);
        });

    QVERIFY(handle != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    QVERIFY2(success, qPrintable(error));
    QCOMPARE(response.content, QStringLiteral("hello"));
    QCOMPARE(response.id, QStringLiteral("msg_test"));
    QCOMPARE(response.model, QStringLiteral("claude-test"));
    QCOMPARE(response.finishReason, QStringLiteral("end_turn"));
    QCOMPARE(response.usage.promptTokens, 7);
    QCOMPARE(response.usage.completionTokens, 5);
    QCOMPARE(response.usage.totalTokens, 12);
    QCOMPARE(response.usage.promptCacheMissTokens, 2);
    QCOMPARE(response.usage.promptCacheHitTokens, 3);
    QCOMPARE(response.usage.cachedTokens, 3);

    QStringList deltas;
    for (const LlmStreamEvent& event : events) {
        if (event.type == LlmStreamEventType::TextDelta) {
            deltas.append(event.textDelta);
        }
    }
    QCOMPARE(deltas, QStringList({QStringLiteral("he"), QStringLiteral("llo")}));
    QCOMPARE(server.requestTargets, QList<QByteArray>{"/v1/messages"});
    QVERIFY(server.requestHeaders.first().contains("x-api-key: test-secret"));
    QVERIFY(server.requestHeaders.first().contains("anthropic-version: 2023-06-01"));
    const QJsonObject payload = QJsonDocument::fromJson(server.requestBodies.first()).object();
    QVERIFY(payload.value("stream").toBool());
    QCOMPARE(payload.value("system").toString(), QStringLiteral("Be concise"));
    const QJsonObject anthropicTool = payload.value("tools").toArray().first().toObject();
    QCOMPARE(anthropicTool.value("name").toString(), QStringLiteral("get_weather"));
    QVERIFY(anthropicTool.value("input_schema").isObject());
}

void TestAnthropicMessagesClient::buildMessages_whenCanonicalImageBlockUsesAnthropic_shouldBuildBase64ImageSource() {
    LocalHttpServer server;
    server.enqueue({200, "text/event-stream", textResponse(), false});
    AnthropicMessagesClient client;
    ChatMessage message;
    message.role = QStringLiteral("user");
    message.contentBlocks = QJsonArray{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), QStringLiteral("inspect")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                    {QStringLiteral("mediaType"), QStringLiteral("image/png")},
                    {QStringLiteral("data"), QStringLiteral("YWJj")}}
    };
    int completions = 0;

    client.sendChatCompletionStreamAsync(
        anthropicConfig(server), {message}, {}, {},
        [&](bool ok, LlmResponse, QString error) {
            QVERIFY2(ok, qPrintable(error));
            ++completions;
        });

    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    const QJsonArray content = QJsonDocument::fromJson(server.requestBodies.first())
                                   .object().value(QStringLiteral("messages"))
                                   .toArray().first().toObject()
                                   .value(QStringLiteral("content")).toArray();
    QCOMPARE(content.size(), 2);
    QCOMPARE(content.at(0).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("inspect"));
    const QJsonObject image = content.at(1).toObject();
    QCOMPARE(image.value(QStringLiteral("type")).toString(),
             QStringLiteral("image"));
    const QJsonObject source = image.value(QStringLiteral("source")).toObject();
    QCOMPARE(source.value(QStringLiteral("type")).toString(),
             QStringLiteral("base64"));
    QCOMPARE(source.value(QStringLiteral("media_type")).toString(),
             QStringLiteral("image/png"));
    QCOMPARE(source.value(QStringLiteral("data")).toString(),
             QStringLiteral("YWJj"));
}

void TestAnthropicMessagesClient::buildMessages_whenCanonicalImageBlockUsesOpenAiCompatible_shouldBuildImageUrlDataUri() {
    LocalHttpServer server;
    const QJsonObject response{
        {QStringLiteral("id"), QStringLiteral("chatcmpl_image")},
        {QStringLiteral("model"), QStringLiteral("vision-model")},
        {QStringLiteral("choices"), QJsonArray{QJsonObject{
             {QStringLiteral("message"), QJsonObject{
                  {QStringLiteral("content"), QStringLiteral("ok")}}},
             {QStringLiteral("finish_reason"), QStringLiteral("stop")}}}}
    };
    server.enqueue({200, "application/json",
                    QJsonDocument(response).toJson(QJsonDocument::Compact), false});
    OpenAICompatibleClient client;
    LlmConfig config = anthropicConfig(server);
    config.provider = QStringLiteral("openai-compatible");
    ChatMessage message;
    message.role = QStringLiteral("user");
    message.contentBlocks = QJsonArray{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), QStringLiteral("inspect")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                    {QStringLiteral("mediaType"), QStringLiteral("image/png")},
                    {QStringLiteral("data"), QStringLiteral("YWJj")}}
    };
    int completions = 0;

    client.sendChatCompletionStreamAsync(
        config, {message}, {}, {},
        [&](bool ok, LlmResponse, QString error) {
            QVERIFY2(ok, qPrintable(error));
            ++completions;
        });

    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    const QJsonArray content = QJsonDocument::fromJson(server.requestBodies.first())
                                   .object().value(QStringLiteral("messages"))
                                   .toArray().first().toObject()
                                   .value(QStringLiteral("content")).toArray();
    QCOMPARE(content.size(), 2);
    QCOMPARE(content.at(0).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("inspect"));
    QCOMPARE(content.at(1).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("image_url"));
    QCOMPARE(content.at(1).toObject().value(QStringLiteral("image_url"))
                 .toObject().value(QStringLiteral("url")).toString(),
             QStringLiteral("data:image/png;base64,YWJj"));
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenAnthropicTextBlockIsInvalid_shouldFailBeforeNetwork() {
    const QList<QJsonArray> invalidContents{
        QJsonArray{QJsonObject{{QStringLiteral("type"),
                                QStringLiteral("text")}}},
        QJsonArray{QJsonObject{{QStringLiteral("type"),
                                QStringLiteral("text")},
                               {QStringLiteral("text"), 42}}}
    };

    for (const QJsonArray& contentBlocks : invalidContents) {
        LocalHttpServer server;
        AnthropicMessagesClient client;
        ChatMessage message;
        message.role = QStringLiteral("user");
        message.contentBlocks = contentBlocks;
        int completions = 0;
        bool success = true;
        QString error;

        client.sendChatCompletionStreamAsync(
            anthropicConfig(server), {message}, {}, {},
            [&](bool ok, LlmResponse, QString detail) {
                success = ok;
                error = std::move(detail);
                ++completions;
            });

        QCOMPARE(completions, 1);
        QVERIFY(!success);
        QVERIFY(error.contains(QStringLiteral("text content block")));
        QCOMPARE(server.requestTargets.size(), 0);
    }
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenOpenAiCanonicalBlockIsInvalid_shouldFailBeforeNetwork() {
    const QList<QJsonArray> invalidContents{
        QJsonArray{QStringLiteral("not-an-object")},
        QJsonArray{QJsonObject{{QStringLiteral("type"),
                                QStringLiteral("unknown")}}},
        QJsonArray{QJsonObject{{QStringLiteral("type"),
                                QStringLiteral("image")},
                               {QStringLiteral("mediaType"),
                                QStringLiteral("image/png")}}}
    };

    for (const QJsonArray& contentBlocks : invalidContents) {
        LocalHttpServer server;
        OpenAICompatibleClient client;
        LlmConfig config = anthropicConfig(server);
        config.provider = QStringLiteral("openai-compatible");
        ChatMessage message;
        message.role = QStringLiteral("user");
        message.contentBlocks = contentBlocks;
        int completions = 0;
        bool success = true;
        QString error;

        client.sendChatCompletionStreamAsync(
            config, {message}, {}, {},
            [&](bool ok, LlmResponse, QString detail) {
                success = ok;
                error = std::move(detail);
                ++completions;
            });

        QCOMPARE(completions, 1);
        QVERIFY(!success);
        QVERIFY(error.contains(QStringLiteral("content block")));
        QCOMPARE(server.requestTargets.size(), 0);
    }
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenBaseUrlHasRootV1OrMessages_shouldNormalizeOneEndpoint() {
    LocalHttpServer server;
    AnthropicMessagesClient client;
    const QStringList suffixes{"", "/v1", "/v1/messages"};
    for (const QString& suffix : suffixes) {
        server.enqueue({200, "text/event-stream", textResponse(), false});
        LlmConfig config = anthropicConfig(server);
        config.baseUrl += suffix;
        int completions = 0;
        client.sendChatCompletionStreamAsync(config, {{"user", "hi", {}, {}, {}}}, {}, {},
            [&](bool ok, LlmResponse, QString error) {
                QVERIFY2(ok, qPrintable(error));
                ++completions;
            });
        QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    }
    QCOMPARE(server.requestTargets, QList<QByteArray>({"/v1/messages", "/v1/messages", "/v1/messages"}));
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenToolInputArrivesInFragments_shouldCompleteOneValidatedToolCall() {
    LocalHttpServer server;
    QByteArray body;
    body += sseEvent("message_start", {{"type", "message_start"}, {"message", QJsonObject{{"id", "msg_tool"}, {"model", "claude-test"}, {"usage", QJsonObject{{"input_tokens", 1}}}}}});
    body += sseEvent("content_block_start", {{"type", "content_block_start"}, {"index", 0}, {"content_block", QJsonObject{{"type", "tool_use"}, {"id", "tool_1"}, {"name", "get_weather"}, {"input", QJsonObject{}}}}});
    body += sseEvent("content_block_delta", {{"type", "content_block_delta"}, {"index", 0}, {"delta", QJsonObject{{"type", "input_json_delta"}, {"partial_json", "{\"city\":"}}}});
    body += sseEvent("content_block_delta", {{"type", "content_block_delta"}, {"index", 0}, {"delta", QJsonObject{{"type", "input_json_delta"}, {"partial_json", "\"Hangzhou\"}"}}}});
    body += sseEvent("content_block_stop", {{"type", "content_block_stop"}, {"index", 0}});
    body += sseEvent("message_delta", {{"type", "message_delta"}, {"delta", QJsonObject{{"stop_reason", "tool_use"}}}, {"usage", QJsonObject{{"output_tokens", 2}}}});
    body += sseEvent("message_stop", {{"type", "message_stop"}});
    server.enqueue({200, "text/event-stream", body, false});

    AnthropicMessagesClient client;
    int completions = 0;
    bool success = false;
    LlmResponse response;
    client.sendChatCompletionStreamAsync(anthropicConfig(server), {{"user", "weather", {}, {}, {}}}, {}, {},
        [&](bool ok, LlmResponse value, QString) {
            success = ok;
            response = std::move(value);
            ++completions;
        });
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    QVERIFY(success);
    QCOMPARE(response.toolCalls.size(), 1);
    QCOMPARE(response.toolCalls.first().id, QStringLiteral("tool_1"));
    QCOMPARE(response.toolCalls.first().name, QStringLiteral("get_weather"));
    QCOMPARE(response.toolCalls.first().arguments.value("city").toString(), QStringLiteral("Hangzhou"));
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenThinkingAndSignatureArrive_shouldKeepThemOutOfVisibleEvents() {
    LocalHttpServer server;
    QByteArray body;
    body += sseEvent("message_start", {{"type", "message_start"}, {"message", QJsonObject{{"id", "msg_thinking"}, {"model", "claude-test"}, {"usage", QJsonObject{{"input_tokens", 1}}}}}});
    body += sseEvent("content_block_start", {{"type", "content_block_start"}, {"index", 0}, {"content_block", QJsonObject{{"type", "thinking"}, {"thinking", ""}}}});
    body += sseEvent("content_block_delta", {{"type", "content_block_delta"}, {"index", 0}, {"delta", QJsonObject{{"type", "thinking_delta"}, {"thinking", "private reasoning"}}}});
    body += sseEvent("content_block_delta", {{"type", "content_block_delta"}, {"index", 0}, {"delta", QJsonObject{{"type", "signature_delta"}, {"signature", "private signature"}}}});
    body += sseEvent("content_block_stop", {{"type", "content_block_stop"}, {"index", 0}});
    body += sseEvent("content_block_start", {{"type", "content_block_start"}, {"index", 1}, {"content_block", QJsonObject{{"type", "text"}, {"text", ""}}}});
    body += sseEvent("content_block_delta", {{"type", "content_block_delta"}, {"index", 1}, {"delta", QJsonObject{{"type", "text_delta"}, {"text", "visible"}}}});
    body += sseEvent("content_block_stop", {{"type", "content_block_stop"}, {"index", 1}});
    body += sseEvent("message_delta", {{"type", "message_delta"}, {"delta", QJsonObject{{"stop_reason", "end_turn"}}}, {"usage", QJsonObject{{"output_tokens", 2}}}});
    body += sseEvent("message_stop", {{"type", "message_stop"}});
    server.enqueue({200, "text/event-stream", body, false});

    AnthropicMessagesClient client;
    QString visible;
    LlmResponse response;
    int completions = 0;
    client.sendChatCompletionStreamAsync(anthropicConfig(server), {{"user", "think", {}, {}, {}}}, {},
        [&](const LlmStreamEvent& event) {
            if (event.type == LlmStreamEventType::TextDelta) {
                visible += event.textDelta;
            }
        },
        [&](bool ok, LlmResponse value, QString error) {
            QVERIFY2(ok, qPrintable(error));
            response = std::move(value);
            ++completions;
        });
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    QCOMPARE(visible, QStringLiteral("visible"));
    QCOMPARE(response.content, QStringLiteral("visible"));
    QVERIFY(!response.content.contains(QStringLiteral("private")));
    QCOMPARE(response.transportBlocks.size(), 2);
    QCOMPARE(response.transportBlocks.first().toObject().value("thinking").toString(), QStringLiteral("private reasoning"));
    QCOMPARE(response.transportBlocks.first().toObject().value("signature").toString(), QStringLiteral("private signature"));
    QCOMPARE(response.transportBlocks.at(1).toObject().value("type").toString(), QStringLiteral("text"));
    QCOMPARE(response.transportBlocks.at(1).toObject().value("text").toString(), QStringLiteral("visible"));
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenContinuationHasTransportBlocks_shouldReplayExactAssistantBlocks() {
    LocalHttpServer server;
    server.enqueue({200, "text/event-stream", textResponse(QStringLiteral("continued")), false});
    const QJsonArray transportBlocks{
        QJsonObject{{"type", "thinking"}, {"thinking", "private"}, {"signature", "signed"}},
        QJsonObject{{"type", "text"}, {"text", "I will check."}},
        QJsonObject{{"type", "tool_use"}, {"id", "tool_7"}, {"name", "lookup"},
                    {"input", QJsonObject{{"query", "weather"}}}}
    };
    ChatMessage assistant;
    assistant.role = QStringLiteral("assistant");
    assistant.content = QStringLiteral("must not be synthesized");
    assistant.toolCalls = QJsonArray{QJsonObject{
        {"id", "duplicate"},
        {"type", "function"},
        {"function", QJsonObject{{"name", "duplicate"}, {"arguments", "{}"}}}
    }};
    assistant.transportBlocks = transportBlocks;

    AnthropicMessagesClient client;
    int completions = 0;
    client.sendChatCompletionStreamAsync(anthropicConfig(server), {assistant}, {}, {},
        [&](bool ok, LlmResponse, QString error) {
            QVERIFY2(ok, qPrintable(error));
            ++completions;
        });
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    const QJsonArray messages = QJsonDocument::fromJson(server.requestBodies.first()).object()
                                    .value("messages").toArray();
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().toObject().value("role").toString(), QStringLiteral("assistant"));
    QCOMPARE(messages.first().toObject().value("content").toArray(), transportBlocks);
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenMessageStopIsMissing_shouldFailExactlyOnce() {
    LocalHttpServer server;
    QByteArray body = textResponse();
    const qsizetype messageStop = body.lastIndexOf("event: message_stop");
    QVERIFY(messageStop >= 0);
    body.truncate(messageStop);
    server.enqueue({200, "text/event-stream", body, false});
    AnthropicMessagesClient client;
    int completions = 0;
    bool success = true;
    QString error;
    client.sendChatCompletionStreamAsync(anthropicConfig(server), {{"user", "hi", {}, {}, {}}}, {}, {},
        [&](bool ok, LlmResponse, QString message) {
            ++completions;
            success = ok;
            error = std::move(message);
        });
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    QVERIFY(!success);
    QVERIFY(error.contains(QStringLiteral("message_stop")));
    QTest::qWait(20);
    QCOMPARE(completions, 1);
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenMessageStopArrivesBeforeMessageStart_shouldFailExactlyOnce() {
    LocalHttpServer server;
    server.enqueue({200, "text/event-stream",
                    sseEvent("message_stop", {{"type", "message_stop"}}), false});
    AnthropicMessagesClient client;
    int completions = 0;
    bool success = true;
    QString error;
    client.sendChatCompletionStreamAsync(anthropicConfig(server), {{"user", "hi", {}, {}, {}}}, {}, {},
        [&](bool ok, LlmResponse, QString message) {
            success = ok;
            error = std::move(message);
            ++completions;
        });
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    QVERIFY(!success);
    QVERIFY(error.contains(QStringLiteral("protocol"), Qt::CaseInsensitive));
    QTest::qWait(20);
    QCOMPARE(completions, 1);
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenContentArrivesAfterMessageStop_shouldFailExactlyOnce() {
    LocalHttpServer server;
    QByteArray body;
    body += sseEvent("message_start", {{"type", "message_start"},
                                        {"message", QJsonObject{{"id", "msg_order"},
                                                                 {"model", "claude-test"},
                                                                 {"usage", QJsonObject{}}}}});
    body += sseEvent("message_stop", {{"type", "message_stop"}});
    body += sseEvent("content_block_start", {{"type", "content_block_start"}, {"index", 0},
                                               {"content_block", QJsonObject{{"type", "text"}, {"text", "late"}}}});
    server.enqueue({200, "text/event-stream", body, false});
    AnthropicMessagesClient client;
    int completions = 0;
    bool success = true;
    QString error;
    client.sendChatCompletionStreamAsync(anthropicConfig(server), {{"user", "hi", {}, {}, {}}}, {}, {},
        [&](bool ok, LlmResponse, QString message) {
            success = ok;
            error = std::move(message);
            ++completions;
        });
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    QVERIFY(!success);
    QVERIFY(error.contains(QStringLiteral("protocol"), Qt::CaseInsensitive));
    QTest::qWait(20);
    QCOMPARE(completions, 1);
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenProviderReturnsHttpError_shouldPreserveSafeDetail() {
    LocalHttpServer server;
    server.enqueue({
        400,
        "application/json",
        QJsonDocument(QJsonObject{
            {"type", "error"},
            {"error", QJsonObject{
                {"type", "invalid_request_error"},
                {"message", "invalid tool schema for test-secret"}
            }}
        }).toJson(QJsonDocument::Compact),
        false
    });
    AnthropicMessagesClient client;
    int completions = 0;
    bool success = true;
    QString error;

    client.sendChatCompletionStreamAsync(
        anthropicConfig(server), {{"user", "hello", {}, {}, {}}}, {}, {},
        [&](bool ok, LlmResponse, QString message) {
            ++completions;
            success = ok;
            error = std::move(message);
        });

    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    QVERIFY(!success);
    QVERIFY(error.contains(QStringLiteral("HTTP 400")));
    QVERIFY(error.contains(QStringLiteral("invalid tool schema")));
    QVERIFY(error.contains(QStringLiteral("[REDACTED]")));
    QVERIFY(!error.contains(QStringLiteral("test-secret")));
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenBaseUrlIsNotAbsoluteHttp_shouldRejectWithoutRequest() {
    LocalHttpServer server;
    AnthropicMessagesClient client;
    const QStringList invalidUrls{
        QStringLiteral("/relative/v1"),
        QStringLiteral("file:///tmp/messages"),
        QStringLiteral("ftp://example.com/v1")
    };
    for (const QString& invalidUrl : invalidUrls) {
        LlmConfig config = anthropicConfig(server);
        config.baseUrl = invalidUrl;
        int completions = 0;
        bool success = true;
        client.sendChatCompletionStreamAsync(config, {{"user", "hi", {}, {}, {}}}, {}, {},
            [&](bool ok, LlmResponse, QString) {
                success = ok;
                ++completions;
            });
        QCOMPARE(completions, 1);
        QVERIFY(!success);
    }
    QCOMPARE(server.requestTargets.size(), 0);
}

void TestAnthropicMessagesClient::cancel_whenReplyIsActive_shouldAbortAndCompleteExactlyOnce() {
    LocalHttpServer server;
    server.enqueue({200, "text/event-stream", {}, true});
    AnthropicMessagesClient client;
    int completions = 0;
    bool success = true;
    QString error;
    const auto handle = client.sendChatCompletionStreamAsync(
        anthropicConfig(server), {{"user", "hi", {}, {}, {}}}, {}, {},
        [&](bool ok, LlmResponse, QString message) {
            ++completions;
            success = ok;
            error = std::move(message);
        });
    QVERIFY(handle != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(server.requestTargets.size(), 1, 2000);
    handle->cancel();
    handle->cancel();
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    QVERIFY(!success);
    QVERIFY(handle->isCancelled());
    QVERIFY(error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
    QTest::qWait(20);
    QCOMPARE(completions, 1);
}

void TestAnthropicMessagesClient::cancel_whenReplyAlreadyFinished_shouldBeIdempotent() {
    LocalHttpServer server;
    server.enqueue({200, "text/event-stream", textResponse(), false});
    AnthropicMessagesClient client;
    int completions = 0;
    const auto handle = client.sendChatCompletionStreamAsync(
        anthropicConfig(server), {{"user", "hi", {}, {}, {}}}, {}, {},
        [&](bool ok, LlmResponse, QString error) {
            QVERIFY2(ok, qPrintable(error));
            ++completions;
        });
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    handle->cancel();
    handle->cancel();
    QCoreApplication::processEvents();
    QCOMPARE(completions, 1);
    QVERIFY(!handle->isCancelled());
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenLegacyCompletionSucceeds_shouldPublishOneDeltaThenComplete() {
    LocalHttpServer server;
    const QJsonObject response{
        {"id", "chatcmpl_test"},
        {"model", "openai-test"},
        {"choices", QJsonArray{QJsonObject{
            {"message", QJsonObject{{"content", "legacy response"}}},
            {"finish_reason", "stop"}
        }}},
        {"usage", QJsonObject{{"prompt_tokens", 2}, {"completion_tokens", 3}, {"total_tokens", 5}}}
    };
    server.enqueue({200, "application/json", QJsonDocument(response).toJson(QJsonDocument::Compact), false});
    OpenAICompatibleClient client;
    LlmConfig config = anthropicConfig(server);
    config.provider = QStringLiteral("openai-compatible");
    QStringList deltas;
    int completions = 0;
    client.sendChatCompletionStreamAsync(config, {{"user", "hi", {}, {}, {}}}, {},
        [&](const LlmStreamEvent& event) {
            if (event.type == LlmStreamEventType::TextDelta) {
                deltas.append(event.textDelta);
            }
        },
        [&](bool ok, LlmResponse value, QString error) {
            QVERIFY2(ok, qPrintable(error));
            QCOMPARE(value.content, QStringLiteral("legacy response"));
            ++completions;
        });
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    QCOMPARE(deltas, QStringList{QStringLiteral("legacy response")});
}

void TestAnthropicMessagesClient::cancel_whenOpenAiReplyIsActive_shouldAbortAndCompleteExactlyOnce() {
    LocalHttpServer server;
    server.enqueue({200, "application/json", {}, true});
    OpenAICompatibleClient client;
    LlmConfig config = anthropicConfig(server);
    config.provider = QStringLiteral("openai-compatible");
    int completions = 0;
    bool success = true;
    const auto handle = client.sendChatCompletionStreamAsync(
        config, {{"user", "hi", {}, {}, {}}}, {}, {},
        [&](bool ok, LlmResponse, QString error) {
            success = ok;
            QVERIFY(error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
            ++completions;
        });
    QTRY_COMPARE_WITH_TIMEOUT(server.requestTargets.size(), 1, 2000);
    handle->cancel();
    handle->cancel();
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(server.clientDisconnects, 1, 2000);
    QVERIFY(!success);
    QVERIFY(handle->isCancelled());
    QCOMPARE(completions, 1);
}

void TestAnthropicMessagesClient::sendChatCompletionStreamAsync_whenOpenAiErrorContainsApiKey_shouldNotExposeIt() {
    LocalHttpServer server;
    server.enqueue({401, "application/json", "{\"error\":\"test-secret\"}", false});
    OpenAICompatibleClient client;
    LlmConfig config = anthropicConfig(server);
    config.provider = QStringLiteral("openai-compatible");
    int completions = 0;
    bool success = true;
    QString error;
    client.sendChatCompletionStreamAsync(config, {{"user", "hi", {}, {}, {}}}, {}, {},
        [&](bool ok, LlmResponse, QString message) {
            success = ok;
            error = std::move(message);
            ++completions;
        });
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 2000);
    QVERIFY(!success);
    QVERIFY(error.contains(QStringLiteral("HTTP 401")));
    QVERIFY(!error.contains(QStringLiteral("test-secret")));
}

void TestAnthropicMessagesClient::cancel_whenLegacyAdapterFinishes_shouldRemainObservableWithEmptyCompletion() {
    DeferredLegacyClient client;
    LlmConfig config;
    const auto handle = client.sendChatCompletionStreamAsync(config, {}, {}, {}, {});
    handle->cancel();
    QVERIFY(handle->isCancelled());
    client.finish();
    QVERIFY(handle->isCancelled());
}

QTEST_MAIN(TestAnthropicMessagesClient)
#include "test_anthropic_messages_client.moc"
