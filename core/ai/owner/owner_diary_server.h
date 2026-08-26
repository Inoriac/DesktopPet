#ifndef DESKTOP_PET_OWNER_DIARY_SERVER_H
#define DESKTOP_PET_OWNER_DIARY_SERVER_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include "ai/domain/domain_result.h"

class OwnerDiaryFacade;
class QLocalServer;
class QLocalSocket;

class OwnerDiaryServer final : public QObject {
    Q_OBJECT

public:
    explicit OwnerDiaryServer(
        OwnerDiaryFacade* facade,
        QString profileId,
        QObject* parent = nullptr);
    ~OwnerDiaryServer() override;

    Result<void, DomainError> listen(
        const QString& socketName,
        const QByteArray& capabilityToken,
        qsizetype maxFrameBytes,
        int sessionTtlSeconds);
    void stop();

    bool isListening() const;
    int sessionCount() const;
    qsizetype bufferedBytesForTesting() const;
    qsizetype cachedResponseBytesForTesting() const;

private:
    struct ConnectionState {
        QByteArray buffer;
        quint32 expectedLength = 0;
        QByteArray sessionToken;
        QDateTime expiresAt;
        QHash<QString, QJsonObject> cachedResponses;
        qsizetype cachedResponseBytes = 0;
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
    void cacheResponse(
        ConnectionState& state,
        const QString& requestId,
        const QJsonObject& response);
    void writeResponse(QLocalSocket* socket, const QJsonObject& response);
    void closeConnection(QLocalSocket* socket);

    OwnerDiaryFacade* m_facade = nullptr;
    QString m_profileId;
    QLocalServer* m_server = nullptr;
    QHash<QLocalSocket*, ConnectionState> m_connections;
    QByteArray m_capabilityToken;
    QString m_socketName;
    qsizetype m_maxFrameBytes = 65536;
    int m_sessionTtlSeconds = 300;
    bool m_capabilityConsumed = false;
};

#endif // DESKTOP_PET_OWNER_DIARY_SERVER_H
