#ifndef DESKTOP_PET_PERSON_DIRECTORY_H
#define DESKTOP_PET_PERSON_DIRECTORY_H

#include <memory>

#include "ai/domain/domain_result.h"
#include "identity_types.h"

class EventOutbox;
class RuntimeUnitOfWork;
class RuntimeUnitOfWorkFactory;
class SqliteIdentityRepository;

// This host service is scoped to one profile. Adapters supply identity data;
// accepted identity-management events are the authority for explicit bindings.
class PersonDirectory {
public:
    PersonDirectory(QString profileId, SqliteIdentityRepository* repository,
                    RuntimeUnitOfWorkFactory* unitOfWorkFactory, EventOutbox* outbox);
    Result<void, DomainError> initialize();
    Result<PersonResolution, DomainError> resolvePerson(const ResolvePersonRequest& request);
    Result<void, DomainError> bindIdentity(const BindIdentityRequest& request);

private:
    Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError> beginWrite();
    QString m_profileId;
    SqliteIdentityRepository* m_repository;
    RuntimeUnitOfWorkFactory* m_unitOfWorkFactory;
    EventOutbox* m_outbox;
};
#endif
