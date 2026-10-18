#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTimer>
#include <QtEndian>

#include "ai/chat/launcher_chat_server.h"
#include "ai/owner/owner_diary_protocol.h"

namespace {
QJsonObject request(const QString& action, const QJsonObject& payload) {
    return {{"protocolVersion", 1}, {"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"action", action}, {"payload", payload}};
}
QJsonObject exchange(QLocalSocket& socket, const QJsonObject& request) {
    socket.write(encodeOwnerDiaryFrame(request));
    QElapsedTimer timer;
    timer.start();
    QByteArray bytes;
    while (timer.elapsed() < 2000) {
        QTest::qWait(1);
        bytes += socket.readAll();
        if (bytes.size() < 4) continue;
        const auto size = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData()));
        if (bytes.size() >= size + 4) return QJsonDocument::fromJson(bytes.mid(4, size)).object();
    }
    return {};
}
LauncherChatCallbacks callbacks() {
    LauncherChatCallbacks value;
    value.snapshot = [] { return QJsonObject{{"messages", QJsonArray{}}}; };
    value.sendMessage = [](const QString&) { return ChatHistoryResult::success({}); };
    value.retryMessage = value.sendMessage;
    value.stopResponse = [] { return Result<void, DomainError>::success(); };
    return value;
}
QString authenticate(QLocalSocket& socket, LauncherChatServer& server, const QString& name) {
    if (!server.listen(name, QByteArray(32, 't'), 1024 * 1024, 300)) return {};
    socket.connectToServer(name);
    if (!socket.waitForConnected(1000)) return {};
    return exchange(socket, request("hello", {{"capabilityToken", ownerDiaryTokenToBase64Url(QByteArray(32, 't'))}}))
        ["data"].toObject()["sessionToken"].toString();
}
}

class TestLauncherChatServer : public QObject {
    Q_OBJECT
private slots:
    void authenticatedQueriesAreAsyncAndDoNotChangeStateRevision();
    void invalidQueriesAndAuthenticationNeverReachStore();
    void lateCompletionAfterDisconnectOrDestructionIsIgnored();
};

void TestLauncherChatServer::authenticatedQueriesAreAsyncAndDoNotChangeStateRevision() {
    auto cb = callbacks();
    int reads = 0;
    bool heartbeat = false;
    cb.queryHistory = [&](const QString& action, const QJsonObject& payload, ChatHistoryCompletion done) {
        ++reads;
        QVERIFY(action == "get_chat_history" || action == "search_chat_history");
        QVERIFY(!payload.contains("sessionToken"));
        QTimer::singleShot(25, [payload, done]() {
            done(ChatHistoryResult::success({{"messages", QJsonArray{}}, {"requestToken", payload["requestToken"]}}));
        });
        QTimer::singleShot(0, [&] { heartbeat = true; });
    };
    LauncherChatServer server("11111111-1111-4111-8111-111111111111", cb);
    QLocalSocket socket;
    const QString token = authenticate(socket, server, "dp-chat-" + QUuid::createUuid().toString(QUuid::Id128).left(12));
    QVERIFY(!token.isEmpty());
    for (const auto& action : {QString("get_chat_history"), QString("search_chat_history")}) {
        QJsonObject payload{{"sessionToken", token}, {"requestToken", "my-query"}};
        if (action == "search_chat_history") payload["query"] = "中文 100%_";
        const auto req = request(action, payload);
        const auto response = exchange(socket, req);
        QVERIFY(response["ok"].toBool());
        QCOMPARE(response["requestId"], req["requestId"]);
        QCOMPARE(response["data"].toObject()["requestToken"].toString(), QString("my-query"));
        QVERIFY(heartbeat);
    }
    QCOMPARE(reads, 2);
    const auto state = exchange(socket, request("get_chat_state", {{"sessionToken", token}, {"afterRevision", 1}}));
    QVERIFY(state["data"].toObject()["unchanged"].toBool());
    QCOMPARE(state["data"].toObject()["openRequestId"].toInt(), 0);
}

void TestLauncherChatServer::invalidQueriesAndAuthenticationNeverReachStore() {
    auto cb = callbacks();
    int reads = 0;
    cb.queryHistory = [&](const QString&, const QJsonObject&, ChatHistoryCompletion) { ++reads; };
    LauncherChatServer server("11111111-1111-4111-8111-111111111111", cb);
    QLocalSocket socket;
    const QString token = authenticate(socket, server, "dp-chat-" + QUuid::createUuid().toString(QUuid::Id128).left(12));
    QVERIFY(!token.isEmpty());
    auto denied = exchange(socket, request("get_chat_history", {{"sessionToken", "forged"}}));
    QCOMPARE(denied["error"].toObject()["code"].toString(), QString("CHAT_AUTH_FAILED"));
    for (auto payload : QList<QJsonObject>{{{"limit", 81}}, {{"aroundMessageId", "a"}, {"beforeMessageId", "b"}}, {{"profileId", "other"}}}) {
        payload["sessionToken"] = token;
        auto invalid = exchange(socket, request("get_chat_history", payload));
        QCOMPARE(invalid["error"].toObject()["code"].toString(), QString("CHAT_QUERY_INVALID"));
    }
    auto empty = exchange(socket, request("search_chat_history", {{"sessionToken", token}, {"query", " "}}));
    QCOMPARE(empty["error"].toObject()["code"].toString(), QString("CHAT_QUERY_INVALID"));
    QCOMPARE(reads, 0);
}

void TestLauncherChatServer::lateCompletionAfterDisconnectOrDestructionIsIgnored() {
    auto cb = callbacks();
    ChatHistoryCompletion pending;
    cb.queryHistory = [&](const QString&, const QJsonObject&, ChatHistoryCompletion done) { pending = done; };
    auto server = std::make_unique<LauncherChatServer>("11111111-1111-4111-8111-111111111111", cb);
    QLocalSocket socket;
    const QString token = authenticate(socket, *server, "dp-chat-" + QUuid::createUuid().toString(QUuid::Id128).left(12));
    QVERIFY(!token.isEmpty());
    socket.write(encodeOwnerDiaryFrame(request("get_chat_history", {{"sessionToken", token}})));
    QTRY_VERIFY(bool(pending));
    socket.abort();
    QTRY_VERIFY(!server->hasAuthenticatedClient());
    pending(ChatHistoryResult::success({}));
    server.reset();
    pending(ChatHistoryResult::success({}));
}

QTEST_GUILESS_MAIN(TestLauncherChatServer)
#include "test_launcher_chat_server.moc"
