#ifndef DESKTOP_PET_PERSONALITY_SERVICE_H
#define DESKTOP_PET_PERSONALITY_SERVICE_H

#include <QString>

#include "ai/domain/domain_result.h"
#include "identity_types.h"

class EventOutbox;
class RuntimeUnitOfWorkFactory;
class SqliteIdentityRepository;

class PersonalityService {
public:
    PersonalityService(QString profileId,
                       IdentityBaseline baseline,
                       PersonalityPolicy policy,
                       SqliteIdentityRepository* repository,
                       RuntimeUnitOfWorkFactory* unitOfWorkFactory,
                       EventOutbox* eventOutbox);

    Result<void, DomainError> recordEvidence(const TraitEvidenceDraft& evidence);
    Result<PersonalitySnapshot, DomainError> consolidate(
        const PersonalityConsolidationRequest& request);
    Result<PersonalitySnapshot, DomainError> rollback(
        const QString& targetStateId,
        int expectedVersion,
        const QString& reason);

private:
    PersonalitySnapshot baselineSnapshot() const;

    QString m_profileId;
    IdentityBaseline m_baseline;
    PersonalityPolicy m_policy;
    SqliteIdentityRepository* m_repository = nullptr;
    RuntimeUnitOfWorkFactory* m_unitOfWorkFactory = nullptr;
    EventOutbox* m_eventOutbox = nullptr;
};

#endif // DESKTOP_PET_PERSONALITY_SERVICE_H
