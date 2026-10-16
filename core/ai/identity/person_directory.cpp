#include "person_directory.h"

#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include "ai/event/event_outbox.h"
#include "sqlite_identity_repository.h"

namespace {
DomainError storeError(const QString& message) {
    return domainError(QStringLiteral("STORE_UNAVAILABLE"), message);
}
DomainError invalid(const QString& message) {
    return domainError(QStringLiteral("INVALID_INPUT"), message);
}
bool validKey(const ExternalIdentity& identity) {
    return !identity.provider.trimmed().isEmpty() && !identity.nameSpace.trimmed().isEmpty()
        && !identity.remoteActorId.trimmed().isEmpty();
}
void bindKey(QSqlQuery& query, const QString& profile, const ExternalIdentity& identity) {
    query.addBindValue(profile);
    query.addBindValue(identity.provider);
    query.addBindValue(identity.nameSpace);
    query.addBindValue(identity.remoteActorId);
}
Result<std::optional<PersonResolution>, DomainError> lookup(
    QSqlDatabase& db, const QString& profile, const ExternalIdentity& identity) {
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT p.person_id,p.status,p.revision FROM agent_external_identities i "
        "JOIN agent_persons p ON p.profile_id=i.profile_id AND p.person_id=i.person_id "
        "WHERE i.profile_id=? AND i.provider=? AND i.namespace=? AND i.remote_actor_id=?"));
    bindKey(query, profile, identity);
    if (!query.exec()) return Result<std::optional<PersonResolution>, DomainError>::failure(storeError(query.lastError().text()));
    if (!query.next()) return Result<std::optional<PersonResolution>, DomainError>::success(std::nullopt);
    PersonResolution result;
    result.personId = query.value(0).toString();
    result.status = query.value(1).toString() == QLatin1String("active")
        ? PersonResolutionStatus::Resolved : PersonResolutionStatus::Provisional;
    result.identityRevision = query.value(2).toLongLong();
    result.externalIdentity = identity;
    return Result<std::optional<PersonResolution>, DomainError>::success(result);
}
Result<void, DomainError> commit(RuntimeUnitOfWork& transaction) {
    const auto result = transaction.commit();
    return result.isOk() ? Result<void, DomainError>::success()
        : Result<void, DomainError>::failure(storeError(result.error().message));
}
QString nonNull(const QString& value) { return value.isNull() ? QStringLiteral("") : value; }
}

PersonDirectory::PersonDirectory(QString profileId, SqliteIdentityRepository* repository,
    RuntimeUnitOfWorkFactory* factory, EventOutbox* outbox)
    : m_profileId(std::move(profileId)), m_repository(repository),
      m_unitOfWorkFactory(factory), m_outbox(outbox) {}

Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError> PersonDirectory::beginWrite() {
    using WriteResult = Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError>;
    if (!isCanonicalEventUuid(m_profileId)) return WriteResult::failure(invalid(QStringLiteral("invalid profile")));
    if (!m_repository || !m_repository->isOpen() || !m_unitOfWorkFactory)
        return WriteResult::failure(storeError(QStringLiteral("identity store is unavailable")));
    auto transaction = m_unitOfWorkFactory->begin();
    if (!transaction.isOk()) return WriteResult::failure(storeError(transaction.error().message));
    if (!transaction.value() || !transaction.value()->isActive())
        return WriteResult::failure(storeError(QStringLiteral("identity transaction is inactive")));
    auto db = QSqlDatabase::database(transaction.value()->connectionName());
    if (!db.isValid() || !db.isOpen() || db.databaseName() != m_repository->databasePath())
        return WriteResult::failure(storeError(QStringLiteral("identity transaction database mismatch")));
    // Acquire the SQLite writer reservation before any reads. Concurrent first
    // resolutions then observe the winner instead of upgrading a stale snapshot.
    QSqlQuery reservation(db);
    reservation.prepare(QStringLiteral("UPDATE agent_persons SET revision=revision WHERE profile_id=? AND 0"));
    reservation.addBindValue(m_profileId);
    if (!reservation.exec()) return WriteResult::failure(storeError(reservation.lastError().text()));
    return transaction;
}

Result<void, DomainError> PersonDirectory::initialize() {
    auto transaction = beginWrite();
    if (!transaction.isOk()) return Result<void, DomainError>::failure(transaction.error());
    auto db = QSqlDatabase::database(transaction.value()->connectionName());
    QSqlQuery query(db);
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO agent_persons(profile_id,person_id,kind,status) VALUES(?,'owner','owner','active')"));
    query.addBindValue(m_profileId);
    if (!query.exec()) return Result<void, DomainError>::failure(storeError(query.lastError().text()));
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO agent_persons(profile_id,person_id,kind,status) "
        "SELECT DISTINCT profile_id,subject_id,'person','provisional' FROM relationship_state "
        "WHERE profile_id=? AND trim(subject_id)<>'' AND subject_id<>'owner'"));
    query.addBindValue(m_profileId);
    if (!query.exec()) return Result<void, DomainError>::failure(storeError(query.lastError().text()));
    return commit(*transaction.value());
}

Result<PersonResolution, DomainError> PersonDirectory::resolvePerson(const ResolvePersonRequest& request) {
    using Resolution = Result<PersonResolution, DomainError>;
    if (request.profileId != m_profileId || !isCanonicalEventUuid(m_profileId)
        || (request.ownerLocal && request.externalIdentity.has_value()))
        return Resolution::failure(invalid(QStringLiteral("invalid identity request context")));
    if (!request.ownerLocal && (!request.externalIdentity.has_value()
        || request.externalIdentity->remoteActorId.trimmed().isEmpty())) {
        PersonResolution unresolved;
        unresolved.externalIdentity = request.externalIdentity;
        return Resolution::success(unresolved);
    }
    if (!request.ownerLocal && !validKey(*request.externalIdentity))
        return Resolution::failure(invalid(QStringLiteral("external identity namespace is missing")));
    auto transaction = beginWrite();
    if (!transaction.isOk()) return Resolution::failure(transaction.error());
    auto db = QSqlDatabase::database(transaction.value()->connectionName());
    PersonResolution resolution;
    QSqlQuery query(db);
    if (request.ownerLocal) {
        query.prepare(QStringLiteral("INSERT OR IGNORE INTO agent_persons(profile_id,person_id,kind,status) VALUES(?,'owner','owner','active')"));
        query.addBindValue(m_profileId);
        if (!query.exec()) return Resolution::failure(storeError(query.lastError().text()));
        query.prepare(QStringLiteral("SELECT revision FROM agent_persons WHERE profile_id=? AND person_id='owner'"));
        query.addBindValue(m_profileId);
        if (!query.exec() || !query.next()) return Resolution::failure(storeError(query.lastError().text()));
        resolution.personId = QStringLiteral("owner");
        resolution.status = PersonResolutionStatus::Resolved;
        resolution.identityRevision = query.value(0).toLongLong();
        query.finish();
    } else {
        const auto& external = *request.externalIdentity;
        auto existing = lookup(db, m_profileId, external);
        if (!existing.isOk()) return Resolution::failure(existing.error());
        if (existing.value().has_value()) {
            resolution = *existing.value();
            query.prepare(QStringLiteral("UPDATE agent_external_identities SET display_name=?,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                "WHERE profile_id=? AND provider=? AND namespace=? AND remote_actor_id=? AND display_name<>?"));
            query.addBindValue(nonNull(external.displayName)); bindKey(query, m_profileId, external);
            query.addBindValue(nonNull(external.displayName));
            if (!query.exec()) return Resolution::failure(storeError(query.lastError().text()));
        } else {
            const QString person = QUuid::createUuid().toString(QUuid::WithoutBraces);
            query.prepare(QStringLiteral("INSERT INTO agent_persons(profile_id,person_id,kind,display_name) VALUES(?,?,'person',?)"));
            query.addBindValue(m_profileId); query.addBindValue(person); query.addBindValue(nonNull(external.displayName));
            if (!query.exec()) return Resolution::failure(storeError(query.lastError().text()));
            query.prepare(QStringLiteral("INSERT INTO agent_external_identities(profile_id,provider,namespace,remote_actor_id,person_id,display_name) VALUES(?,?,?,?,?,?)"));
            bindKey(query, m_profileId, external); query.addBindValue(person); query.addBindValue(nonNull(external.displayName));
            if (!query.exec()) {
                const auto failure = query.lastError();
                query.finish(); transaction.value()->rollback();
                // Unique-key competition must never leave an orphan person.
                if (failure.nativeErrorCode() == QLatin1String("19")) {
                    auto retry = lookup(db, m_profileId, external);
                    if (retry.isOk() && retry.value().has_value()) return Resolution::success(*retry.value());
                }
                return Resolution::failure(storeError(failure.text()));
            }
            resolution = {person, PersonResolutionStatus::Provisional, 1, external};
        }
    }
    const auto committed = commit(*transaction.value());
    if (!committed.isOk()) return Resolution::failure(committed.error());
    return Resolution::success(resolution);
}

Result<void, DomainError> PersonDirectory::bindIdentity(const BindIdentityRequest& request) {
    using Binding = Result<void, DomainError>;
    if (request.profileId != m_profileId || !validKey(request.externalIdentity)
        || request.personId.trimmed().isEmpty() || request.expectedRevision <= 0)
        return Binding::failure(invalid(QStringLiteral("invalid identity binding request")));
    auto transaction = beginWrite();
    if (!transaction.isOk()) return Binding::failure(transaction.error());
    auto db = QSqlDatabase::database(transaction.value()->connectionName());
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT person_id,binding_evidence_ref FROM agent_external_identities "
        "WHERE profile_id=? AND provider=? AND namespace=? AND remote_actor_id=?"));
    bindKey(query, m_profileId, request.externalIdentity);
    if (!query.exec()) return Binding::failure(storeError(query.lastError().text()));
    const bool bound = query.next();
    if (bound && query.value(0).toString() != request.personId)
        return Binding::failure(domainError(QStringLiteral("IDENTITY_CONFLICT"), QStringLiteral("account already belongs to another person")));
    if (bound && !request.bindingEvidenceRef.isEmpty() && query.value(1).toString() == request.bindingEvidenceRef)
        return Binding::success();
    query.finish();
    if (request.bindingEvidenceRef.trimmed().isEmpty()) return Binding::failure(invalid(QStringLiteral("binding evidence is missing")));
    query.prepare(QStringLiteral("SELECT source,type,payload_json FROM event_log WHERE profile_id=? AND event_id=?"));
    query.addBindValue(m_profileId); query.addBindValue(request.bindingEvidenceRef);
    if (!query.exec()) return Binding::failure(storeError(query.lastError().text()));
    if (!query.next()) return Binding::failure(domainError(QStringLiteral("UNKNOWN_REFERENCE"), QStringLiteral("binding evidence is not committed")));
    const auto payload = QJsonDocument::fromJson(query.value(2).toByteArray()).object();
    if (query.value(0).toString() != QLatin1String("IdentityManagement")
        || query.value(1).toString() != QLatin1String("IdentityBindingAccepted")
        || payload.value(QStringLiteral("person_id")).toString() != request.personId
        || payload.value(QStringLiteral("provider")).toString() != request.externalIdentity.provider
        || payload.value(QStringLiteral("namespace")).toString() != request.externalIdentity.nameSpace
        || payload.value(QStringLiteral("remote_actor_id")).toString() != request.externalIdentity.remoteActorId
        || payload.value(QStringLiteral("expected_revision")).toInteger() != request.expectedRevision)
        return Binding::failure(domainError(QStringLiteral("UNKNOWN_REFERENCE"), QStringLiteral("binding evidence does not match request")));
    query.finish();
    query.prepare(QStringLiteral("SELECT revision FROM agent_persons WHERE profile_id=? AND person_id=?"));
    query.addBindValue(m_profileId); query.addBindValue(request.personId);
    if (!query.exec()) return Binding::failure(storeError(query.lastError().text()));
    if (!query.next()) return Binding::failure(domainError(QStringLiteral("UNKNOWN_REFERENCE"), QStringLiteral("target person does not exist")));
    if (query.value(0).toLongLong() != request.expectedRevision)
        return Binding::failure(domainError(QStringLiteral("STALE_REVISION"), QStringLiteral("person identity revision has changed")));
    query.finish();
    // A previously bound account cannot acquire a replacement evidence reference
    // through this add-account API. Identity correction is a separate operation.
    if (bound) return Binding::failure(domainError(QStringLiteral("IDENTITY_CONFLICT"), QStringLiteral("account binding already has a different origin")));
    query.prepare(QStringLiteral("INSERT INTO agent_external_identities(profile_id,provider,namespace,remote_actor_id,person_id,display_name,binding_evidence_ref) VALUES(?,?,?,?,?,?,?)"));
    bindKey(query, m_profileId, request.externalIdentity); query.addBindValue(request.personId);
    query.addBindValue(nonNull(request.externalIdentity.displayName)); query.addBindValue(request.bindingEvidenceRef);
    if (!query.exec()) return Binding::failure(storeError(query.lastError().text()));
    query.prepare(QStringLiteral("UPDATE agent_persons SET revision=revision+1,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE profile_id=? AND person_id=? AND revision=?"));
    query.addBindValue(m_profileId); query.addBindValue(request.personId); query.addBindValue(request.expectedRevision);
    if (!query.exec()) return Binding::failure(storeError(query.lastError().text()));
    if (query.numRowsAffected() != 1) return Binding::failure(domainError(QStringLiteral("STALE_REVISION"), QStringLiteral("person identity revision has changed")));
    if (!m_outbox) return Binding::failure(storeError(QStringLiteral("identity outbox is unavailable")));
    EventDraft event;
    event.profileId = m_profileId;
    event.type = QStringLiteral("IdentityBound");
    event.source = QStringLiteral("PersonDirectory");
    event.payload = {{QStringLiteral("person_id"), request.personId},
        {QStringLiteral("provider"), request.externalIdentity.provider},
        {QStringLiteral("namespace"), request.externalIdentity.nameSpace},
        {QStringLiteral("remote_actor_id"), request.externalIdentity.remoteActorId},
        {QStringLiteral("identity_revision"), request.expectedRevision + 1},
        {QStringLiteral("binding_evidence_ref"), request.bindingEvidenceRef}};
    const auto queued = m_outbox->enqueue(*transaction.value(), event);
    if (!queued.isOk()) return Binding::failure(storeError(queued.error().message));
    return commit(*transaction.value());
}
