#ifndef DESKTOP_PET_IDENTITY_TYPES_H
#define DESKTOP_PET_IDENTITY_TYPES_H

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

#include "identity_baseline.h"

struct PersonalityPolicy {
    int minimumIndependentEvidence = 3;
    int minimumContextKeys = 2;
    int minimumWindowDays = 14;
    double maxDeltaPerWindow = 0.05;
    int maxPromptSlotChars = 512;
};

PersonalityPolicy sanitizePersonalityPolicy(const PersonalityPolicy& policy);

enum class TraitEvidenceStatus {
    Pending,
    Consumed,
    Rejected
};

struct TraitEvidenceDraft {
    QString trait;
    double direction = 0.0;
    double weight = 0.0;
    double confidence = 0.0;
    QString contextKey;
    QString sourceEventId;
    QDateTime createdAt;
};

struct TraitEvidence : TraitEvidenceDraft {
    QString evidenceId;
    QString profileId;
    TraitEvidenceStatus status = TraitEvidenceStatus::Pending;
};

struct PersonalityConsolidationRequest {
    QString profileId;
    QDateTime windowStart;
    QDateTime windowEnd;
    int expectedVersion = 0;
};

struct PersonalitySnapshot {
    QString stateId;
    QString profileId;
    qint64 version = 0;
    IdentityBaseline baseline = IdentityBaseline::defaults();
    QMap<QString, double> tendencies;
    QStringList sourceEvidenceIds;
    qint64 evidenceCutoffSequence = 0;
    QDateTime effectiveAt;
    QDateTime createdAt;
};

struct RelationshipEvidence {
    QString tendency;
    double direction = 0.0;
    double weight = 0.0;
    double confidence = 0.0;
    QString sourceEventId;
    QDateTime occurredAt;
};

struct RelationshipSnapshot {
    QString stateId;
    QString profileId;
    QString subjectId;
    qint64 version = 0;
    QMap<QString, double> tendencies;
    QStringList sourceEvidenceIds;
    qint64 evidenceCutoffSequence = 0;
    QDateTime effectiveAt;
    QDateTime createdAt;
};

enum class EvidenceSourceType {
    CommittedEvent,
    DiaryIndex,
    PersonalityVersion,
    SelfModel
};

struct SelfModelEvidence {
    QString referenceId;
    EvidenceSourceType sourceType = EvidenceSourceType::CommittedEvent;
    bool committed = false;
};

struct EvidenceSet {
    QList<SelfModelEvidence> items;
};

struct SelfModelProposal {
    QString narrative;
    std::optional<QString> parentVersionId;
    QDateTime proposedAt;
};

struct SelfModelSnapshot {
    QString versionId;
    QString profileId;
    std::optional<QString> parentVersionId;
    QString narrative;
    EvidenceSet evidence;
    QDateTime effectiveAt;
    QDateTime createdAt;
};

struct InteractionContext {
    QString petName;
    QDateTime at;
};

QString traitEvidenceStatusToString(TraitEvidenceStatus status);
std::optional<TraitEvidenceStatus> traitEvidenceStatusFromString(const QString& value);
QString evidenceSourceTypeToString(EvidenceSourceType type);
std::optional<EvidenceSourceType> evidenceSourceTypeFromString(const QString& value);

QJsonObject numericMapToJson(const QMap<QString, double>& values);
QMap<QString, double> numericMapFromJson(const QJsonObject& object);
QJsonObject relationshipSnapshotStateToJson(const RelationshipSnapshot& snapshot);
RelationshipSnapshot relationshipSnapshotStateFromJson(const QJsonObject& object);
QJsonArray evidenceSetToJson(const EvidenceSet& evidence);
EvidenceSet evidenceSetFromJson(const QJsonArray& array);

#endif // DESKTOP_PET_IDENTITY_TYPES_H
