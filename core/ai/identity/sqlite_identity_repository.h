#ifndef DESKTOP_PET_SQLITE_IDENTITY_REPOSITORY_H
#define DESKTOP_PET_SQLITE_IDENTITY_REPOSITORY_H

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

#include "ai/domain/domain_result.h"
#include "ai/event/runtime_unit_of_work.h"
#include "identity_types.h"

class SqliteIdentityRepository {
public:
    SqliteIdentityRepository() = default;
    virtual ~SqliteIdentityRepository();

    SqliteIdentityRepository(const SqliteIdentityRepository&) = delete;
    SqliteIdentityRepository& operator=(const SqliteIdentityRepository&) = delete;

    Result<void, DomainError> open(const QString& databasePath);
    void close();
    bool isOpen() const;
    const QString& databasePath() const { return m_databasePath; }

    Result<void, DomainError> insertEvidence(const TraitEvidence& evidence);
    Result<QList<TraitEvidence>, DomainError> evidenceBySource(
        const QString& profileId, const QString& sourceEventId) const;
    Result<QList<TraitEvidence>, DomainError> pendingEvidence(
        const QString& profileId,
        const QDateTime& from,
        const QDateTime& to) const;

    Result<std::optional<PersonalitySnapshot>, DomainError> currentPersonality(
        const QString& profileId) const;
    Result<std::optional<PersonalitySnapshot>, DomainError> personalityAt(
        const QString& profileId, qint64 version) const;
    Result<std::optional<PersonalitySnapshot>, DomainError> personalityByStateId(
        const QString& profileId, const QString& stateId) const;
    Result<QList<PersonalitySnapshot>, DomainError> personalityHistory(
        const QString& profileId) const;
    virtual Result<void, DomainError> appendPersonalityState(
        RuntimeUnitOfWork& unitOfWork,
        const PersonalitySnapshot& snapshot,
        const QStringList& consumedEvidenceIds);

    Result<std::optional<RelationshipSnapshot>, DomainError> currentRelationship(
        const QString& profileId, const QString& subjectId) const;
    Result<std::optional<RelationshipSnapshot>, DomainError> relationshipAt(
        const QString& profileId,
        const QString& subjectId,
        qint64 version) const;
    Result<void, DomainError> appendRelationshipState(
        const RelationshipSnapshot& snapshot);

    Result<std::optional<SelfModelSnapshot>, DomainError> currentSelfModel(
        const QString& profileId) const;
    Result<std::optional<SelfModelSnapshot>, DomainError> selfModelAt(
        const QString& profileId, const QString& versionId) const;
    Result<void, DomainError> appendSelfModelState(
        const SelfModelSnapshot& snapshot);

private:
    QString m_databasePath;
    QString m_connectionName;
};

#endif // DESKTOP_PET_SQLITE_IDENTITY_REPOSITORY_H
