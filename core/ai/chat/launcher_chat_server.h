#ifndef DESKTOP_PET_LAUNCHER_CHAT_SERVER_H
#define DESKTOP_PET_LAUNCHER_CHAT_SERVER_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

#include "ai/domain/domain_result.h"

class QLocalServer;
class QLocalSocket;

struct LauncherChatCallbacks {
    std::function<QJsonObject()> snapshot;
    std::function<Result<QJsonObject, DomainError>(const QString&)> sendMessage;
    std::function<Result<QJsonObject, DomainError>(const QString&)> retryMessage;
    std::function<Result<void, DomainError>()> stopResponse;
};

class LauncherChatServer final : public QObject {
    Q_OBJECT

public:
    explicit LauncherChatServer(
        QString profileId,
        LauncherChatCallbacks callbacks,
        QObject* parent = nullptr);
    ~LauncherChatServer() override;

    Result<void, DomainError> listen(
        const QString& socketName,
        const QByteArray& capabilityToken,
        qsizetype maxFrameBytes,
        int sessionTtlSeconds);
    void stop();
    bool isListening() const;

    void notifyStateChanged();
    void requestOpenInLauncher();

private:
    struct ConnectionState {
        QByteArray buffer;
        quint32 expectedLength = 0;
        QByteArray sessionToken;
        QDateTime expiresAt;
        qint64 requestWindowStartedAtMs = 0;
        int requestCount = 0;
    };

    void acceptConnections();
    void readFrames(QLocalSocket* socket);
    void processFrame(QLocalSocket* socket, const QByteArray& payload);
    QJsonObject handleRequest(QLocalSocket* socket, const QJsonObject& request);
    QJsonObject handleHello(
        QLocalSocket* socket,
        const QString& requestId,
        const QJsonObject& payload);
    bool authorize(
        QLocalSocket* socket,
        const QJsonObject& payload,
        QJsonObject* errorResponse,
        const QString& requestId);
    bool consumeRequestBudget(ConnectionState& state);
    void writeResponse(QLocalSocket* socket, const QJsonObject& response);
    void closeConnection(QLocalSocket* socket);

    QString m_profileId;
    LauncherChatCallbacks m_callbacks;
    QLocalServer* m_server = nullptr;
    QHash<QLocalSocket*, ConnectionState> m_connections;
    QByteArray m_capabilityToken;
    QString m_socketName;
    qsizetype m_maxFrameBytes = 1024 * 1024;
    int m_sessionTtlSeconds = 300;
    quint64 m_revision = 1;
    quint64 m_openRequestId = 0;
};

#endif // DESKTOP_PET_LAUNCHER_CHAT_SERVER_H
