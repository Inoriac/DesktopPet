#ifndef DESKTOP_PET_EVENT_OUTBOX_H
#define DESKTOP_PET_EVENT_OUTBOX_H

#include "event_types.h"
#include "runtime_unit_of_work.h"

class EventSchemaRegistry;

class EventOutbox {
public:
    virtual ~EventOutbox() = default;
    virtual Result<QString, DomainError> enqueue(
        RuntimeUnitOfWork& unitOfWork, const EventDraft& draft) = 0;
    virtual Result<int, DomainError> dispatchPending(int limit) = 0;
};

class SqliteEventOutbox final : public EventOutbox {
public:
    SqliteEventOutbox(QString databasePath,
                      const EventSchemaRegistry* schemas,
                      QString profileId);

    Result<QString, DomainError> enqueue(
        RuntimeUnitOfWork& unitOfWork, const EventDraft& draft) override;
    Result<int, DomainError> dispatchPending(int limit) override;

private:
    QString m_databasePath;
    const EventSchemaRegistry* m_schemas = nullptr;
    QString m_profileId;
};

#endif // DESKTOP_PET_EVENT_OUTBOX_H
