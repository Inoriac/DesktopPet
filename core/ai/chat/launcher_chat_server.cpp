#include "launcher_chat_server.h"

#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <iostream>
#include <utility>

#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "ai/owner/owner_diary_protocol.h"

namespace {

constexpr int kMaxConnections = 4;
constexpr int kHelloTimeoutMs = 3000;
constexpr int kRequestsPerMinute = 600;

bool peerIsCurrentUser(QLocalSocket* socket) {
#if defined(Q_OS_LINUX)
    struct ucred credentials {};
    socklen_t size = sizeof(credentials);
    return ::getsockopt(static_cast<int>(socket->socketDescriptor()), SOL_SOCKET,
                        SO_PEERCRED, &credentials, &size) == 0
        && credentials.uid == ::geteuid();
#elif defined(Q_OS_MACOS)
    uid_t uid = 0;
    gid_t gid = 0;
    return ::getpeereid(static_cast<int>(socket->socketDescriptor()), &uid, &gid) == 0
        && uid == ::geteuid();
#else
    Q_UNUSED(socket)
    return true;
#endif
}

QJsonObject resultError(const QString& requestId, const DomainError& error) {
    return ownerDiaryErrorResponse(
        requestId,
        error.code.isEmpty() ? QStringLiteral("CHAT_REQUEST_FAILED") : error.code,
        error.message.isEmpty() ? QStringLiteral("Chat request failed") : error.message);
}

} // namespace

LauncherChatServer::LauncherChatServer(
    QString profileId,
    LauncherChatCallbacks callbacks,
    QObject* parent)
    : QObject(parent),
      m_profileId(std::move(profileId)),
      m_callbacks(std::move(callbacks)),
      m_server(new QLocalServer(this)) {
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection,
            this, &LauncherChatServer::acceptConnections);
}

LauncherChatServer::~LauncherChatServer() {
    stop();
}

Result<void, DomainError> LauncherChatServer::listen(
    const QString& socketName,
    const QByteArray& capabilityToken,
    qsizetype maxFrameBytes,
    int sessionTtlSeconds) {
    stop();
    if (m_profileId.trimmed().isEmpty() || socketName.trimmed().isEmpty()
        || capabilityToken.size() != 32 || !m_callbacks.snapshot
        || !m_callbacks.sendMessage || !m_callbacks.retryMessage
        || !m_callbacks.stopResponse) {
        return Result<void, DomainError>::failure(domainError(
            QStringLiteral("CHAT_BRIDGE_INVALID"),
            QStringLiteral("Launcher chat bridge configuration is invalid")));
    }

    m_socketName = socketName;
    m_capabilityToken = capabilityToken;
    m_maxFrameBytes = std::clamp<qsizetype>(
        maxFrameBytes, kOwnerDiaryMinFrameBytes, kOwnerDiaryMaxFrameBytes);
    m_sessionTtlSeconds = std::clamp(
        sessionTtlSeconds,
        kOwnerDiaryMinSessionTtlSeconds,
        kOwnerDiaryMaxSessionTtlSeconds);
    QLocalServer::removeServer(socketName);
    if (!m_server->listen(socketName)) {
        std::cerr << "[LauncherChat] listener failed: "
                  << m_server->errorString().toStdString() << std::endl;
        m_capabilityToken.fill('\0');
        m_capabilityToken.clear();
        return Result<void, DomainError>::failure(domainError(
            QStringLiteral("CHAT_BRIDGE_UNAVAILABLE"),
            QStringLiteral("Launcher chat bridge could not listen")));
    }
    std::cerr << "[LauncherChat] listener started; socket suffix="
              << socketName.right(8).toStdString() << std::endl;
    return Result<void, DomainError>::success();
}

void LauncherChatServer::stop() {
    const QList<QLocalSocket*> sockets = m_connections.keys();
    for (QLocalSocket* socket : sockets) closeConnection(socket);
    if (m_server->isListening()) m_server->close();
    if (!m_socketName.isEmpty()) QLocalServer::removeServer(m_socketName);
    m_socketName.clear();
    m_capabilityToken.fill('\0');
    m_capabilityToken.clear();
}

bool LauncherChatServer::isListening() const {
    return m_server && m_server->isListening();
}

void LauncherChatServer::notifyStateChanged() {
    ++m_revision;
}

void LauncherChatServer::requestOpenInLauncher() {
    ++m_openRequestId;
    notifyStateChanged();
}

void LauncherChatServer::acceptConnections() {
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        if (m_connections.size() >= kMaxConnections || !peerIsCurrentUser(socket)) {
            socket->disconnectFromServer();
            socket->deleteLater();
            continue;
        }
        m_connections.insert(socket, {});
        std::cerr << "[LauncherChat] local client connected" << std::endl;
        connect(socket, &QLocalSocket::readyRead,
                this, [this, socket]() { readFrames(socket); });
        connect(socket, &QLocalSocket::disconnected,
                this, [this, socket]() { closeConnection(socket); });
        connect(socket, &QLocalSocket::errorOccurred,
                this, [socket](QLocalSocket::LocalSocketError error) {
                    std::cerr << "[LauncherChat] local socket error="
                              << static_cast<int>(error) << " message="
                              << socket->errorString().toStdString() << std::endl;
                });
        QTimer::singleShot(kHelloTimeoutMs, socket, [this, socket]() {
            const auto found = m_connections.constFind(socket);
            if (found != m_connections.cend() && found->sessionToken.isEmpty()) {
                closeConnection(socket);
            }
        });
    }
}

void LauncherChatServer::readFrames(QLocalSocket* socket) {
    auto found = m_connections.find(socket);
    if (found == m_connections.end()) return;
    found->buffer.append(socket->readAll());
    if (found->buffer.size() > m_maxFrameBytes + 4) {
        closeConnection(socket);
        return;
    }

    while (true) {
        if (found->expectedLength == 0) {
            if (found->buffer.size() < 4) return;
            found->expectedLength = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar*>(found->buffer.constData()));
            found->buffer.remove(0, 4);
            if (found->expectedLength == 0
                || found->expectedLength > static_cast<quint32>(m_maxFrameBytes)) {
                closeConnection(socket);
                return;
            }
        }
        if (found->buffer.size() < static_cast<qsizetype>(found->expectedLength)) {
            return;
        }
        const QByteArray payload = found->buffer.left(found->expectedLength);
        found->buffer.remove(0, found->expectedLength);
        found->expectedLength = 0;
        processFrame(socket, payload);
        found = m_connections.find(socket);
        if (found == m_connections.end()) return;
    }
}

void LauncherChatServer::processFrame(
    QLocalSocket* socket,
    const QByteArray& payload) {
    const auto parsed = parseOwnerDiaryJson(payload);
    if (!parsed.isOk()) {
        writeResponse(socket, ownerDiaryErrorResponse(
            {}, QStringLiteral("PROTOCOL_INVALID"),
            QStringLiteral("Launcher chat frame is invalid")));
        return;
    }
    writeResponse(socket, handleRequest(socket, parsed.value()));
}

QJsonObject LauncherChatServer::handleRequest(
    QLocalSocket* socket,
    const QJsonObject& request) {
    const QString requestId = request.value(QStringLiteral("requestId")).toString();
    const QString action = request.value(QStringLiteral("action")).toString();
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    if (request.value(QStringLiteral("protocolVersion")).toInt(-1)
            != kOwnerDiaryProtocolVersion
        || QUuid(requestId).isNull() || action.trimmed().isEmpty()
        || !request.value(QStringLiteral("payload")).isObject()) {
        return ownerDiaryErrorResponse(
            requestId, QStringLiteral("PROTOCOL_INVALID"),
            QStringLiteral("Launcher chat request envelope is invalid"));
    }
    if (action == QLatin1String("hello")) {
        return handleHello(socket, requestId, payload);
    }

    QJsonObject authError;
    if (!authorize(socket, payload, &authError, requestId)) return authError;
    ConnectionState& state = m_connections[socket];
    if (!consumeRequestBudget(state)) {
        return ownerDiaryErrorResponse(
            requestId, QStringLiteral("RATE_LIMITED"),
            QStringLiteral("Launcher chat request rate exceeded"), true);
    }

    if (action == QLatin1String("get_chat_state")) {
        QJsonObject data{
            {QStringLiteral("revision"), static_cast<qint64>(m_revision)},
            {QStringLiteral("openRequestId"), static_cast<qint64>(m_openRequestId)}
        };
        const qint64 afterRevision =
            payload.value(QStringLiteral("afterRevision")).toInteger(-1);
        if (afterRevision == static_cast<qint64>(m_revision)) {
            data.insert(QStringLiteral("unchanged"), true);
        } else {
            const QJsonObject snapshot = m_callbacks.snapshot();
            for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
                data.insert(it.key(), it.value());
            }
            data.insert(QStringLiteral("unchanged"), false);
        }
        return ownerDiarySuccessResponse(requestId, data);
    }
    if (action == QLatin1String("send_message")) {
        const auto result = m_callbacks.sendMessage(
            payload.value(QStringLiteral("text")).toString());
        if (!result.isOk()) return resultError(requestId, result.error());
        notifyStateChanged();
        return ownerDiarySuccessResponse(requestId, result.value());
    }
    if (action == QLatin1String("retry_message")) {
        const auto result = m_callbacks.retryMessage(
            payload.value(QStringLiteral("messageId")).toString());
        if (!result.isOk()) return resultError(requestId, result.error());
        notifyStateChanged();
        return ownerDiarySuccessResponse(requestId, result.value());
    }
    if (action == QLatin1String("stop_response")) {
        const auto result = m_callbacks.stopResponse();
        if (!result.isOk()) return resultError(requestId, result.error());
        notifyStateChanged();
        return ownerDiarySuccessResponse(requestId, {});
    }
    return ownerDiaryErrorResponse(
        requestId, QStringLiteral("ACTION_UNSUPPORTED"),
        QStringLiteral("Launcher chat action is unsupported"));
}

QJsonObject LauncherChatServer::handleHello(
    QLocalSocket* socket,
    const QString& requestId,
    const QJsonObject& payload) {
    auto found = m_connections.find(socket);
    const QByteArray provided = ownerDiaryTokenFromBase64Url(
        payload.value(QStringLiteral("capabilityToken")).toString());
    const bool hasAuthenticatedPeer = std::any_of(
        m_connections.cbegin(), m_connections.cend(),
        [](const ConnectionState& state) {
            return !state.sessionToken.isEmpty()
                && state.expiresAt > QDateTime::currentDateTimeUtc();
        });
    if (found == m_connections.end() || hasAuthenticatedPeer
        || !found->sessionToken.isEmpty()
        || provided.size() != 32
        || !ownerDiaryConstantTimeEquals(provided, m_capabilityToken)) {
        std::cerr << "[LauncherChat] hello rejected" << std::endl;
        return ownerDiaryErrorResponse(
            requestId, QStringLiteral("CHAT_AUTH_FAILED"),
            QStringLiteral("Launcher chat authentication failed"));
    }
    found->sessionToken = ownerDiaryRandomToken();
    found->expiresAt = QDateTime::currentDateTimeUtc().addSecs(m_sessionTtlSeconds);
    std::cerr << "[LauncherChat] hello accepted" << std::endl;
    return ownerDiarySuccessResponse(requestId, {
        {QStringLiteral("sessionToken"),
         ownerDiaryTokenToBase64Url(found->sessionToken)},
        {QStringLiteral("serverVersion"), kOwnerDiaryProtocolVersion},
        {QStringLiteral("profileId"), m_profileId}
    });
}

bool LauncherChatServer::authorize(
    QLocalSocket* socket,
    const QJsonObject& payload,
    QJsonObject* errorResponse,
    const QString& requestId) {
    auto found = m_connections.find(socket);
    const QByteArray provided = ownerDiaryTokenFromBase64Url(
        payload.value(QStringLiteral("sessionToken")).toString());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (found == m_connections.end() || found->sessionToken.isEmpty()
        || found->expiresAt <= now
        || !ownerDiaryConstantTimeEquals(provided, found->sessionToken)) {
        *errorResponse = ownerDiaryErrorResponse(
            requestId, QStringLiteral("CHAT_AUTH_FAILED"),
            QStringLiteral("Launcher chat session is invalid"));
        return false;
    }
    found->expiresAt = now.addSecs(m_sessionTtlSeconds);
    return true;
}

bool LauncherChatServer::consumeRequestBudget(ConnectionState& state) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (state.requestWindowStartedAtMs == 0
        || now - state.requestWindowStartedAtMs >= 60000) {
        state.requestWindowStartedAtMs = now;
        state.requestCount = 0;
    }
    return ++state.requestCount <= kRequestsPerMinute;
}

void LauncherChatServer::writeResponse(
    QLocalSocket* socket,
    const QJsonObject& response) {
    QByteArray frame = encodeOwnerDiaryFrame(response);
    if (frame.size() > m_maxFrameBytes + 4) {
        frame = encodeOwnerDiaryFrame(ownerDiaryErrorResponse(
            response.value(QStringLiteral("requestId")).toString(),
            QStringLiteral("RESPONSE_TOO_LARGE"),
            QStringLiteral("Launcher chat response is too large")));
    }
    socket->write(frame);
    socket->flush();
}

void LauncherChatServer::closeConnection(QLocalSocket* socket) {
    auto found = m_connections.find(socket);
    if (found != m_connections.end()) {
        found->sessionToken.fill('\0');
        found->sessionToken.clear();
        m_connections.erase(found);
    }
    socket->disconnect(this);
    if (socket->state() != QLocalSocket::UnconnectedState) socket->abort();
    socket->deleteLater();
}
