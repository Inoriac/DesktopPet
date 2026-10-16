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

// Populated by trusted host input adapters, never by message text.
enum class OriginKind { LocalOwner, External, Internal };
QString originKindToString(OriginKind kind);
std::optional<OriginKind> originKindFromString(const QString& value);

struct ExternalIdentity {
    QString provider;
    QString nameSpace;
    QString remoteActorId;
    QString displayName;
};

enum class PersonResolutionStatus { Resolved, Provisional, Unresolved };
struct ResolvePersonRequest {
    QString profileId;
    std::optional<ExternalIdentity> externalIdentity;
    bool ownerLocal = false;
};
struct PersonResolution {
    std::optional<QString> personId;
    PersonResolutionStatus status = PersonResolutionStatus::Unresolved;
    qint64 identityRevision = 0;
    std::optional<ExternalIdentity> externalIdentity;
};
struct BindIdentityRequest {
    QString profileId;
    ExternalIdentity externalIdentity;
    QString personId;
    qint64 expectedRevision = 0;
    QString bindingEvidenceRef;
};

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

enum class TraitEvidenceCategory { Fact, Feeling, Speculation };
QString traitEvidenceCategoryToString(TraitEvidenceCategory category);
std::optional<TraitEvidenceCategory> traitEvidenceCategoryFromString(const QString& value);
double traitConsolidationLimit(const QString& trait);
QMap<QString, double> relationshipDefaults();

struct TraitEvidenceDraft {
    QString trait;
    double direction = 0.0;
    double weight = 0.0;
    double confidence = 0.0;
    QString contextKey;
    QString sourceEventId;
    QDateTime createdAt;
    // Derived diaries/reflections retain the original experience identity and time.
    QString originalSourceId;
    QString sourceCategory = QStringLiteral("interaction");
    TraitEvidenceCategory category = TraitEvidenceCategory::Fact;
    QDateTime originalOccurredAt;
    qint64 sourceSequence = 0;
};

struct TraitEvidence : TraitEvidenceDraft {
    QString evidenceId;
    QString profileId;
    TraitEvidenceStatus status = TraitEvidenceStatus::Pending;
    qint64 consumedVersion = 0;
    qint64 revision = 1;
};

struct PersonalityConsolidationRequest {
    QString profileId;
    QDateTime windowStart;
    QDateTime windowEnd;
    int expectedVersion = 0;
    qint64 evidenceCutoffSequence = 0;
};

struct PersonalitySnapshot {
    QString stateId;
    QString profileId;
    qint64 version = 0;
    IdentityBaseline baseline = IdentityBaseline::defaults();
    QMap<QString, double> tendencies;
    QStringList sourceEvidenceIds;
    QMap<QString, qint64> sourceEvidenceRevisions;
    qint64 evidenceCutoffSequence = 0;
    QDateTime effectiveAt;
    QDateTime createdAt;
    qint64 parentVersion = 0;
    QString behaviorStyleSummary;
    QString selfNarrativeSummary;

    QMap<QString, double> effectiveTraits() const;
};

struct PersonalityConsolidationPreview {
    PersonalityConsolidationRequest request;
    PersonalitySnapshot current;
    PersonalitySnapshot candidate;
    QStringList consumedEvidenceIds;
    bool hasChanges = false;
};

struct RelationshipEvidence {
    QString tendency;
    double direction = 0.0;
    double weight = 0.0;
    double confidence = 0.0;
    QString sourceEventId;
    QDateTime occurredAt;
    // Absence and the pet declining a task are not negative relationship evidence.
    QString eventKind = QStringLiteral("interaction");
};

struct RelationshipSnapshot {
    QString stateId;
    QString profileId;
    QString subjectId;
    qint64 version = 0;
    QMap<QString, double> tendencies = relationshipDefaults();
    // UTC date + dimension -> accumulated absolute movement, persisted across restarts.
    QMap<QString, double> dailyAbsoluteChanges;
    QStringList sourceEvidenceIds;
    qint64 evidenceCutoffSequence = 0;
    QDateTime effectiveAt;
    QDateTime createdAt;

    double favorability() const;
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
