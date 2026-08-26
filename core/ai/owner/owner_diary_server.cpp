#include "owner_diary_server.h"

#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSet>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <utility>

#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "owner_diary_facade.h"
#include "owner_diary_protocol.h"

namespace {

constexpr int kOwnerDiaryMaxConnections = 16;
constexpr int kOwnerDiaryHelloTimeoutMs = 3000;
constexpr int kOwnerDiaryRequestsPerMinute = 120;
constexpr qsizetype kOwnerDiaryMaxCachedBytes = 256 * 1024;

bool hasOnlyKeys(const QJsonObject& object, const QSet<QString>& allowed) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowed.contains(it.key())) return false;
    }
    return true;
}

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

QJsonObject metadataJson(const DiaryMetadata& metadata) {
    return {
        {QStringLiteral("entryId"), metadata.entryId},
        {QStringLiteral("localDate"), metadata.localDate.toString(Qt::ISODate)},
        {QStringLiteral("index"), metadata.index},
        {QStringLiteral("createdAt"),
         metadata.createdAt.toUTC().toString(Qt::ISODateWithMs)}
    };
}

QJsonObject entryJson(const DiaryEntry& entry) {
    return {
        {QStringLiteral("entryId"), entry.entryId},
        {QStringLiteral("localDate"), entry.localDate.toString(Qt::ISODate)},
        {QStringLiteral("body"), entry.body},
        {QStringLiteral("index"), entry.index},
        {QStringLiteral("createdAt"),
         entry.createdAt.toUTC().toString(Qt::ISODateWithMs)}
    };
}

} // namespace

OwnerDiaryServer::OwnerDiaryServer(
    OwnerDiaryFacade* facade,
    QString profileId,
    QObject* parent)
    : QObject(parent),
      m_facade(facade),
      m_profileId(std::move(profileId)),
      m_server(new QLocalServer(this)) {
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection,
            this, &OwnerDiaryServer::acceptConnections);
}

OwnerDiaryServer::~OwnerDiaryServer() {
    stop();
}

Result<void, DomainError> OwnerDiaryServer::listen(
    const QString& socketName,
    const QByteArray& capabilityToken,
    qsizetype maxFrameBytes,
    int sessionTtlSeconds) {
    stop();
    if (!m_facade || m_profileId.trimmed().isEmpty()
        || socketName.trimmed().isEmpty() || capabilityToken.size() != 32) {
        return Result<void, DomainError>::failure(
            domainError(QStringLiteral("OWNER_AUTH_FAILED"),
                        QStringLiteral("owner diary listener configuration is invalid")));
    }
    m_socketName = socketName;
    m_capabilityToken = QByteArray(
        capabilityToken.constData(), capabilityToken.size());
    m_maxFrameBytes = std::clamp<qsizetype>(
        maxFrameBytes, kOwnerDiaryMinFrameBytes, kOwnerDiaryMaxFrameBytes);
    m_sessionTtlSeconds = std::clamp(
        sessionTtlSeconds,
        kOwnerDiaryMinSessionTtlSeconds,
        kOwnerDiaryMaxSessionTtlSeconds);
    m_capabilityConsumed = false;
    if (!m_server->listen(m_socketName)) {
        m_capabilityToken.fill('\0');
        m_capabilityToken.clear();
        m_socketName.clear();
        return Result<void, DomainError>::failure(
            domainError(QStringLiteral("OWNER_AUTH_FAILED"),
                        QStringLiteral("owner diary socket cannot listen")));
    }
    return Result<void, DomainError>::success();
}

void OwnerDiaryServer::stop() {
    const QList<QLocalSocket*> sockets = m_connections.keys();
    for (QLocalSocket* socket : sockets) closeConnection(socket);
    m_connections.clear();
    if (m_server && m_server->isListening()) m_server->close();
    m_capabilityToken.fill('\0');
    m_capabilityToken.clear();
    m_socketName.clear();
    m_capabilityConsumed = false;
}

bool OwnerDiaryServer::isListening() const {
    return m_server && m_server->isListening();
}

int OwnerDiaryServer::sessionCount() const {
    int count = 0;
    for (const ConnectionState& state : m_connections) {
        if (!state.sessionToken.isEmpty()) ++count;
    }
    return count;
}

qsizetype OwnerDiaryServer::bufferedBytesForTesting() const {
    qsizetype total = 0;
    for (const ConnectionState& state : m_connections) {
        total += state.buffer.size();
    }
    return total;
}

qsizetype OwnerDiaryServer::cachedResponseBytesForTesting() const {
    qsizetype total = 0;
    for (const ConnectionState& state : m_connections) {
        total += state.cachedResponseBytes;
    }
    return total;
}

void OwnerDiaryServer::acceptConnections() {
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        socket->setParent(this);
        if (m_connections.size() >= kOwnerDiaryMaxConnections
            || !peerIsCurrentUser(socket)) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        socket->setReadBufferSize(m_maxFrameBytes + 4);
        m_connections.insert(socket, ConnectionState{});
        connect(socket, &QLocalSocket::readyRead,
                this, [this, socket]() { readFrames(socket); });
        connect(socket, &QLocalSocket::disconnected,
                this, [this, socket]() { closeConnection(socket); });
        QTimer::singleShot(kOwnerDiaryHelloTimeoutMs, socket, [this, socket]() {
            const auto state = m_connections.constFind(socket);
            if (state != m_connections.cend() && state->sessionToken.isEmpty()) {
                closeConnection(socket);
            }
        });
    }
}

void OwnerDiaryServer::readFrames(QLocalSocket* socket) {
    auto state = m_connections.find(socket);
    if (state == m_connections.end()) return;
    const QByteArray incoming = socket->readAll();
    if (incoming.size() > m_maxFrameBytes + 4
        || state->buffer.size() > m_maxFrameBytes + 4 - incoming.size()) {
        closeConnection(socket);
        return;
    }
    state->buffer.append(incoming);
    while (m_connections.contains(socket)) {
        state = m_connections.find(socket);
        if (state->expectedLength == 0) {
            if (state->buffer.size() < 4) return;
            state->expectedLength = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar*>(state->buffer.constData()));
            state->buffer.remove(0, 4);
            if (state->expectedLength == 0
                || state->expectedLength > static_cast<quint32>(m_maxFrameBytes)) {
                closeConnection(socket);
                return;
            }
        }
        if (state->buffer.size() < static_cast<int>(state->expectedLength)) return;
        const QByteArray payload = state->buffer.left(state->expectedLength);
        state->buffer.remove(0, state->expectedLength);
        state->expectedLength = 0;
        processFrame(socket, payload);
    }
}

void OwnerDiaryServer::processFrame(
    QLocalSocket* socket,
    const QByteArray& payload) {
    const auto parsed = parseOwnerDiaryJson(payload);
    if (!parsed.isOk()) {
        closeConnection(socket);
        return;
    }
    auto state = m_connections.find(socket);
    const QString requestId = parsed.value()
                                  .value(QStringLiteral("requestId")).toString();
    if (state == m_connections.end() || !consumeRequestBudget(*state)) {
        writeResponse(socket, ownerDiaryErrorResponse(
            requestId.left(128), QStringLiteral("OWNER_RATE_LIMITED"),
            QStringLiteral("owner diary request rate exceeded"), true));
        socket->disconnectFromServer();
        return;
    }
    const QJsonObject response = handleRequest(socket, parsed.value());
    writeResponse(socket, response);
    const QString errorCode = response.value(QStringLiteral("error"))
                                  .toObject().value(QStringLiteral("code")).toString();
    if (!response.value(QStringLiteral("ok")).toBool()
        && errorCode == QLatin1String("OWNER_AUTH_FAILED")) {
        socket->disconnectFromServer();
    }
}

QJsonObject OwnerDiaryServer::handleRequest(
    QLocalSocket* socket,
    const QJsonObject& request) {
    const QString requestId = request.value(QStringLiteral("requestId")).toString();
    if (request.value(QStringLiteral("protocolVersion")).toInt(-1)
        != kOwnerDiaryProtocolVersion) {
        return ownerDiaryErrorResponse(
            requestId, QStringLiteral("PROTOCOL_VERSION_UNSUPPORTED"),
            QStringLiteral("owner diary protocol version is unsupported"));
    }
    if (requestId.trimmed().isEmpty() || requestId.size() > 128
        || !request.value(QStringLiteral("payload")).isObject()) {
        return ownerDiaryErrorResponse(
            requestId, QStringLiteral("PROTOCOL_INVALID"),
            QStringLiteral("owner diary request is invalid"));
    }
    const QString action = request.value(QStringLiteral("action")).toString();
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    if (action == QLatin1String("hello")) {
        return handleHello(socket, requestId, payload);
    }

    auto state = m_connections.find(socket);
    if (state == m_connections.end()) {
        return ownerDiaryErrorResponse(
            requestId, QStringLiteral("OWNER_AUTH_FAILED"),
            QStringLiteral("owner diary session is unavailable"));
    }
    QJsonObject authError;
    if (!authorize(socket, payload, &authError, requestId)) return authError;
    if (state->cachedResponses.contains(requestId)) {
        return state->cachedResponses.value(requestId);
    }

    QJsonObject response;
    const OwnerAuthContext auth{m_profileId, true};
    if (action == QLatin1String("list_diary_entries")) {
        static const QSet<QString> allowed{
            QStringLiteral("sessionToken"), QStringLiteral("from"),
            QStringLiteral("to"), QStringLiteral("cursor"),
            QStringLiteral("limit")};
        if (!hasOnlyKeys(payload, allowed)) {
            response = ownerDiaryErrorResponse(
                requestId, QStringLiteral("ACTION_NOT_ALLOWED"),
                QStringLiteral("owner diary payload field is not allowed"));
            cacheResponse(*state, requestId, response);
            return response;
        }
        DiaryListQuery query;
        const QString from = payload.value(QStringLiteral("from")).toString();
        const QString to = payload.value(QStringLiteral("to")).toString();
        if (!from.isEmpty()) query.from = QDate::fromString(from, Qt::ISODate);
        if (!to.isEmpty()) query.to = QDate::fromString(to, Qt::ISODate);
        query.cursor = payload.value(QStringLiteral("cursor")).toString();
        query.limit = payload.value(QStringLiteral("limit")).toInt(20);
        if ((!from.isEmpty() && !query.from.isValid())
            || (!to.isEmpty() && !query.to.isValid())) {
            response = ownerDiaryErrorResponse(
                requestId, QStringLiteral("DIARY_QUERY_INVALID"),
                QStringLiteral("diary date range is invalid"));
        } else {
            const auto result = m_facade->list(query, auth);
            if (!result.isOk()) {
                response = ownerDiaryErrorResponse(
                    requestId, result.error().code, result.error().message);
            } else {
                QJsonArray entries;
                for (const DiaryMetadata& metadata : result.value().entries) {
                    entries.append(metadataJson(metadata));
                }
                response = ownerDiarySuccessResponse(requestId, {
                    {QStringLiteral("entries"), entries},
                    {QStringLiteral("nextCursor"), result.value().nextCursor}
                });
            }
        }
    } else if (action == QLatin1String("get_diary_entry")) {
        static const QSet<QString> allowed{
            QStringLiteral("sessionToken"), QStringLiteral("entryId")};
        if (!hasOnlyKeys(payload, allowed)) {
            response = ownerDiaryErrorResponse(
                requestId, QStringLiteral("ACTION_NOT_ALLOWED"),
                QStringLiteral("owner diary payload field is not allowed"));
            cacheResponse(*state, requestId, response);
            return response;
        }
        const auto result = m_facade->get(
            payload.value(QStringLiteral("entryId")).toString(), auth);
        response = result.isOk()
            ? ownerDiarySuccessResponse(requestId, entryJson(result.value()))
            : ownerDiaryErrorResponse(
                  requestId, result.error().code, result.error().message);
    } else {
        response = ownerDiaryErrorResponse(
            requestId, QStringLiteral("ACTION_NOT_ALLOWED"),
            QStringLiteral("owner diary action is not allowed"));
    }
    cacheResponse(*state, requestId, response);
    return response;
}

QJsonObject OwnerDiaryServer::handleHello(
    QLocalSocket* socket,
    const QString& requestId,
    const QJsonObject& payload) {
    auto state = m_connections.find(socket);
    const QByteArray supplied = ownerDiaryTokenFromBase64Url(
        payload.value(QStringLiteral("capabilityToken")).toString());
    const QString nonce = payload.value(QStringLiteral("clientNonce")).toString();
    static const QSet<QString> allowed{
        QStringLiteral("capabilityToken"), QStringLiteral("clientNonce")};
    if (state == m_connections.end() || m_capabilityConsumed
        || !hasOnlyKeys(payload, allowed)
        || nonce.trimmed().isEmpty() || nonce.size() > 256
        || supplied.size() != 32
        || !ownerDiaryConstantTimeEquals(supplied, m_capabilityToken)) {
        return ownerDiaryErrorResponse(
            requestId, QStringLiteral("OWNER_AUTH_FAILED"),
            QStringLiteral("owner diary authentication failed"));
    }
    state->sessionToken = ownerDiaryRandomToken();
    state->expiresAt = QDateTime::currentDateTimeUtc().addSecs(m_sessionTtlSeconds);
    m_capabilityConsumed = true;
    m_capabilityToken.fill('\0');
    m_capabilityToken.clear();
    QTimer::singleShot(m_sessionTtlSeconds * 1000, socket, [this, socket]() {
        const auto current = m_connections.constFind(socket);
        if (current != m_connections.cend()
            && current->expiresAt <= QDateTime::currentDateTimeUtc()) {
            closeConnection(socket);
        }
    });
    return ownerDiarySuccessResponse(requestId, {
        {QStringLiteral("sessionToken"),
         ownerDiaryTokenToBase64Url(state->sessionToken)},
        {QStringLiteral("serverVersion"), kOwnerDiaryProtocolVersion}
    });
}

bool OwnerDiaryServer::authorize(
    QLocalSocket* socket,
    const QJsonObject& payload,
    QJsonObject* errorResponse,
    const QString& requestId) {
    auto state = m_connections.find(socket);
    const QByteArray supplied = ownerDiaryTokenFromBase64Url(
        payload.value(QStringLiteral("sessionToken")).toString());
    const bool expired = state != m_connections.end()
        && state->expiresAt <= QDateTime::currentDateTimeUtc();
    if (expired) {
        state->sessionToken.fill('\0');
        state->sessionToken.clear();
        state->cachedResponses.clear();
        state->cachedResponseBytes = 0;
    }
    if (state == m_connections.end() || state->sessionToken.size() != 32
        || expired
        || supplied.size() != 32
        || !ownerDiaryConstantTimeEquals(supplied, state->sessionToken)) {
        *errorResponse = ownerDiaryErrorResponse(
            requestId, QStringLiteral("OWNER_AUTH_FAILED"),
            QStringLiteral("owner diary session is invalid or expired"));
        return false;
    }
    return true;
}

bool OwnerDiaryServer::consumeRequestBudget(ConnectionState& state) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (state.requestWindowStartedAtMs == 0
        || now - state.requestWindowStartedAtMs >= 60000) {
        state.requestWindowStartedAtMs = now;
        state.requestCount = 1;
        return true;
    }
    ++state.requestCount;
    return state.requestCount <= kOwnerDiaryRequestsPerMinute;
}

void OwnerDiaryServer::cacheResponse(
    ConnectionState& state,
    const QString& requestId,
    const QJsonObject& response) {
    const qsizetype payloadBytes = encodeOwnerDiaryFrame(response).size() - 4;
    const qsizetype byteBudget = std::min(
        kOwnerDiaryMaxCachedBytes, m_maxFrameBytes * 2);
    if (payloadBytes <= 0 || payloadBytes > m_maxFrameBytes
        || payloadBytes > byteBudget) {
        return;
    }
    if (state.cachedResponses.size() >= 128
        || state.cachedResponseBytes + payloadBytes > byteBudget) {
        state.cachedResponses.clear();
        state.cachedResponseBytes = 0;
    }
    state.cachedResponses.insert(requestId, response);
    state.cachedResponseBytes += payloadBytes;
}

void OwnerDiaryServer::writeResponse(
    QLocalSocket* socket,
    const QJsonObject& response) {
    if (!socket || socket->state() != QLocalSocket::ConnectedState) return;
    QByteArray frame = encodeOwnerDiaryFrame(response);
    if (frame.size() - 4 > m_maxFrameBytes) {
        const QString requestId = response.value(QStringLiteral("requestId"))
                                      .toString().left(128);
        frame = encodeOwnerDiaryFrame(ownerDiaryErrorResponse(
            requestId, QStringLiteral("DIARY_RESPONSE_TOO_LARGE"),
            QStringLiteral("owner diary response exceeds the frame limit")));
    }
    socket->write(frame);
    socket->flush();
}

void OwnerDiaryServer::closeConnection(QLocalSocket* socket) {
    if (!socket) return;
    auto state = m_connections.find(socket);
    if (state != m_connections.end()) {
        state->sessionToken.fill('\0');
        state->sessionToken.clear();
        state->buffer.fill('\0');
        state->buffer.clear();
        state->cachedResponses.clear();
        state->cachedResponseBytes = 0;
        m_connections.erase(state);
    }
    socket->abort();
    socket->deleteLater();
}
