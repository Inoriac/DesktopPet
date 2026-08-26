#ifndef DESKTOP_PET_REFLECTION_TYPES_H
#define DESKTOP_PET_REFLECTION_TYPES_H

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

#include "ai/domain/domain_result.h"
#include "ai/event/event_types.h"
#include "ai/integration/emotion_state_provider.h"
#include "ai_types.h"

struct InnerThoughtRequest {
    QString profileId;
    QString sourceEventId;
    QJsonObject contextSnapshot;
    std::optional<ProvidedEmotionSnapshot> emotionSnapshot;
    EventPrivacy privacy = EventPrivacy::Private;
};

struct InnerThoughtSummary {
    QString entryId;
    QString profileId;
    QString sourceEventId;
    QString appraisal;
    QString desire;
    QString uncertainty;
    bool unresolved = false;
    QDateTime createdAt;
};

struct DaydreamRequest {
    QString profileId;
    QString sessionId;
    qint64 sourceCutoffSequence = 0;
    int maxItems = 32;
};

struct DiaryRequest {
    QString profileId;
    QString sessionId;
    QDate localDate;
    qint64 sourceCutoffSequence = 0;
    QStringList eventSummaries;
    QList<ProvidedEmotionSnapshot> emotionTrajectory;
    QStringList innerThoughtRefs;
    QStringList committedMemorySummaries;
    QString petName;
};

struct DiaryEntry {
    QString entryId;
    QString profileId;
    QDate localDate;
    QString body;
    QJsonObject index;
    qint64 sourceCutoffSequence = 0;
    int keyVersion = 1;
    QDateTime createdAt;
};

struct DiaryListQuery {
    QDate from;
    QDate to;
    QString cursor;
    int limit = 20;
};

struct DiaryMetadata {
    QString entryId;
    QDate localDate;
    QJsonObject index;
    QDateTime createdAt;
};

struct DiaryPage {
    QList<DiaryMetadata> entries;
    QString nextCursor;
};

struct OwnerAuthContext {
    QString profileId;
    bool authenticated = false;
};

struct StagingSession {
    QString sessionId;
    quint64 generation = 0;
};

enum class SleepTriggerType {
    Bedtime,
    Manual,
    Recovery
};

struct SleepTrigger {
    SleepTriggerType type = SleepTriggerType::Bedtime;
    int observedIdleSeconds = 0;
    QDateTime now;
    QString profileId;
};

enum class SleepCancelReason {
    UserInteraction,
    TaskDueSoon,
    Shutdown,
    DependencyFailure
};

enum class SleepDecision {
    Pending,
    Commit,
    Abort
};

enum class SleepSessionState {
    Snapshotting,
    Consolidating,
    Journaling,
    Committing,
    Sleeping,
    Cancelling,
    RolledBack,
    Completed
};

struct SleepSessionRecord {
    QString sessionId;
    QString profileId;
    qint64 sourceCutoffSequence = 0;
    SleepSessionState state = SleepSessionState::Snapshotting;
    SleepDecision decision = SleepDecision::Pending;
    QStringList participants{QStringLiteral("memory"),
                             QStringLiteral("private_psyche")};
    QStringList finalizedParticipants;
    int revision = 1;
    QDateTime startedAt;
    QDateTime updatedAt;
    QDateTime decisionAt;
    QDateTime completedAt;
};

struct PrivateKeyMaterial {
    QString profileId;
    int keyVersion = 1;
    QByteArray key;
};

struct PrivateRecordAad {
    int schemaVersion = 1;
    QString profileId;
    QString recordType;
    QString recordId;
    int keyVersion = 1;

    QByteArray toBytes() const;
};

struct EncryptedPrivatePayload {
    int schemaVersion = 1;
    int keyVersion = 1;
    QByteArray nonce;
    QByteArray ciphertext;
};

struct StoredPrivateRecord {
    QString recordId;
    QString profileId;
    QString recordType;
    QDate localDate;
    QString sourceEventId;
    QJsonObject index;
    qint64 sourceCutoffSequence = 0;
    EncryptedPrivatePayload encrypted;
    QDateTime createdAt;
};

QString sleepDecisionToString(SleepDecision decision);
std::optional<SleepDecision> sleepDecisionFromString(const QString& value);
QString sleepSessionStateToString(SleepSessionState state);
std::optional<SleepSessionState> sleepSessionStateFromString(const QString& value);

using InnerThoughtHandler =
    std::function<void(Result<QString, DomainError>)>;
using DiaryHandler =
    std::function<void(Result<QString, DomainError>)>;

#endif // DESKTOP_PET_REFLECTION_TYPES_H
