#ifndef DESKTOP_PET_EVENT_LEDGER_H
#define DESKTOP_PET_EVENT_LEDGER_H

#include <QList>

#include "event_types.h"

class EventSchemaRegistry;
class SqliteEventRepository;

class EventLedger {
public:
    virtual ~EventLedger() = default;
    virtual Result<EventRecord, DomainError> append(const EventDraft& draft) = 0;
    virtual Result<QList<EventRecord>, DomainError> readAfter(
        qint64 sequence, const EventFilter& filter, int limit) const = 0;
};

class EventConsumerCheckpointStore {
public:
    virtual ~EventConsumerCheckpointStore() = default;
    virtual Result<qint64, DomainError> current(const QString& consumerId) const = 0;
    virtual Result<void, DomainError> commit(
        const QString& consumerId, qint64 expectedSequence, qint64 nextSequence) = 0;
};

class SqliteEventLedger final : public EventLedger {
public:
    SqliteEventLedger(SqliteEventRepository* repository,
                      const EventSchemaRegistry* schemas,
                      QString profileId);

    Result<EventRecord, DomainError> append(const EventDraft& draft) override;
    Result<QList<EventRecord>, DomainError> readAfter(
        qint64 sequence, const EventFilter& filter, int limit) const override;

private:
    SqliteEventRepository* m_repository = nullptr;
    const EventSchemaRegistry* m_schemas = nullptr;
    QString m_profileId;
};

class SqliteEventConsumerCheckpointStore final : public EventConsumerCheckpointStore {
public:
    explicit SqliteEventConsumerCheckpointStore(SqliteEventRepository* repository)
        : m_repository(repository) {}

    Result<qint64, DomainError> current(const QString& consumerId) const override;
    Result<void, DomainError> commit(
        const QString& consumerId, qint64 expectedSequence, qint64 nextSequence) override;

private:
    SqliteEventRepository* m_repository = nullptr;
};

#endif // DESKTOP_PET_EVENT_LEDGER_H
