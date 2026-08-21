#include "event_ledger.h"

#include <QDateTime>
#include <QUuid>

#include "event_schema_registry.h"
#include "sqlite_event_repository.h"

#include <utility>

SqliteEventLedger::SqliteEventLedger(SqliteEventRepository* repository,
                                     const EventSchemaRegistry* schemas,
                                     QString profileId)
    : m_repository(repository)
    , m_schemas(schemas)
    , m_profileId(std::move(profileId)) {}

Result<EventRecord, DomainError> SqliteEventLedger::append(const EventDraft& draft) {
    if (!m_repository || !m_schemas || draft.profileId != m_profileId) {
        return Result<EventRecord, DomainError>::failure(
            domainError(QStringLiteral("EVT_SCHEMA_INVALID"),
                        QStringLiteral("event does not belong to this runtime")));
    }
    EventDraft finalDraft = draft;
    if (finalDraft.eventId.isEmpty()) {
        finalDraft.eventId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    }
    if (!finalDraft.occurredAt.isValid()) {
        finalDraft.occurredAt = QDateTime::currentDateTimeUtc();
    } else {
        finalDraft.occurredAt = finalDraft.occurredAt.toUTC();
    }
    Result<void, DomainError> validation = m_schemas->validate(finalDraft);
    if (!validation.isOk()) {
        return Result<EventRecord, DomainError>::failure(validation.error());
    }
    return m_repository->append(finalDraft);
}

Result<QList<EventRecord>, DomainError> SqliteEventLedger::readAfter(
    qint64 sequence, const EventFilter& filter, int limit) const {
    if (!m_repository || filter.authorization.profileId() != m_profileId
        || filter.authorization.consumerId().trimmed().isEmpty()) {
        return Result<QList<EventRecord>, DomainError>::failure(
            domainError(QStringLiteral("EVT_READ_FORBIDDEN"),
                        QStringLiteral("event read authorization does not match runtime")));
    }
    if (sequence < 0 || limit <= 0) {
        return Result<QList<EventRecord>, DomainError>::failure(
            domainError(QStringLiteral("EVT_SCHEMA_INVALID"),
                        QStringLiteral("event read cursor or limit is invalid")));
    }
    return m_repository->readAfter(sequence, filter, limit);
}

Result<qint64, DomainError> SqliteEventConsumerCheckpointStore::current(
    const QString& consumerId) const {
    if (!m_repository) {
        return Result<qint64, DomainError>::failure(
            domainError(QStringLiteral("EVENT_OUTBOX_UNAVAILABLE"),
                        QStringLiteral("checkpoint repository is unavailable")));
    }
    return m_repository->currentCheckpoint(consumerId);
}

Result<void, DomainError> SqliteEventConsumerCheckpointStore::commit(
    const QString& consumerId, qint64 expectedSequence, qint64 nextSequence) {
    if (!m_repository) {
        return Result<void, DomainError>::failure(
            domainError(QStringLiteral("EVENT_OUTBOX_UNAVAILABLE"),
                        QStringLiteral("checkpoint repository is unavailable")));
    }
    return m_repository->commitCheckpoint(consumerId, expectedSequence, nextSequence);
}
