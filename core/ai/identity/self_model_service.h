#ifndef DESKTOP_PET_SELF_MODEL_SERVICE_H
#define DESKTOP_PET_SELF_MODEL_SERVICE_H

#include <QString>

#include "ai/domain/domain_result.h"
#include "identity_types.h"

class SqliteIdentityRepository;

class SelfModelService {
public:
    SelfModelService(QString profileId, SqliteIdentityRepository* repository);

    Result<SelfModelSnapshot, DomainError> evolve(
        const SelfModelProposal& proposal, const EvidenceSet& evidence);

private:
    QString m_profileId;
    SqliteIdentityRepository* m_repository = nullptr;
};

#endif // DESKTOP_PET_SELF_MODEL_SERVICE_H
