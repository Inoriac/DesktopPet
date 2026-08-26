#ifndef DESKTOP_PET_OWNER_DIARY_PROTOCOL_H
#define DESKTOP_PET_OWNER_DIARY_PROTOCOL_H

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include "ai/domain/domain_result.h"

constexpr int kOwnerDiaryProtocolVersion = 1;
constexpr int kOwnerDiaryMinFrameBytes = 1024;
constexpr int kOwnerDiaryMaxFrameBytes = 1024 * 1024;
constexpr int kOwnerDiaryMinSessionTtlSeconds = 30;
constexpr int kOwnerDiaryMaxSessionTtlSeconds = 3600;

struct OwnerDiaryBootstrap {
    int protocolVersion = kOwnerDiaryProtocolVersion;
    QString profileId;
    QString socketName;
    QByteArray capabilityToken;
    QDateTime expiresAt;
    qsizetype maxFrameBytes = 65536;
    int sessionTtlSeconds = 300;
};

Result<OwnerDiaryBootstrap, DomainError> consumeOwnerDiaryBootstrap(
    const QString& absolutePath,
    const QString& expectedProfileId);

QByteArray encodeOwnerDiaryFrame(const QJsonObject& object);
Result<QJsonObject, DomainError> parseOwnerDiaryJson(const QByteArray& payload);
QByteArray ownerDiaryRandomToken();
QByteArray ownerDiaryTokenFromBase64Url(const QString& encoded);
QString ownerDiaryTokenToBase64Url(const QByteArray& token);
bool ownerDiaryConstantTimeEquals(const QByteArray& left, const QByteArray& right);

QJsonObject ownerDiarySuccessResponse(
    const QString& requestId,
    const QJsonObject& data);
QJsonObject ownerDiaryErrorResponse(
    const QString& requestId,
    const QString& code,
    const QString& message,
    bool retryable = false);

#endif // DESKTOP_PET_OWNER_DIARY_PROTOCOL_H
