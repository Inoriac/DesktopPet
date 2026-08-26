#ifndef DESKTOP_PET_RELATIONSHIP_SERVICE_H
#define DESKTOP_PET_RELATIONSHIP_SERVICE_H

#include <QString>

#include "ai/domain/domain_result.h"
#include "identity_types.h"

class SqliteIdentityRepository;

class RelationshipService {
public:
    RelationshipService(QString profileId, SqliteIdentityRepository* repository);

    Result<RelationshipSnapshot, DomainError> applyEvidence(
        const QString& subjectId, const RelationshipEvidence& evidence);

private:
    QString m_profileId;
    SqliteIdentityRepository* m_repository = nullptr;
};

#endif // DESKTOP_PET_RELATIONSHIP_SERVICE_H
