#ifndef DESKTOP_PET_SQLITE_EVENT_REPOSITORY_H
#define DESKTOP_PET_SQLITE_EVENT_REPOSITORY_H

#include <QSqlDatabase>
#include <QString>

#include "event_types.h"

class SqliteEventRepository {
public:
    SqliteEventRepository();
    ~SqliteEventRepository();

    Result<void, DomainError> open(const QString& databasePath);
    void close();
    bool isOpen() const;
    const QString& databasePath() const { return m_databasePath; }
    const QString& connectionName() const { return m_connectionName; }

    Result<EventRecord, DomainError> append(const EventDraft& draft);
    Result<QList<EventRecord>, DomainError> readAfter(
        qint64 sequence, const EventFilter& filter, int limit) const;
    Result<qint64, DomainError> currentCheckpoint(const QString& consumerId) const;
    Result<void, DomainError> commitCheckpoint(
        const QString& consumerId, qint64 expectedSequence, qint64 nextSequence);

    static Result<EventRecord, DomainError> insertEvent(
        QSqlDatabase& database, const EventDraft& draft, bool idempotent);

private:
    Result<void, DomainError> migrateSchema(QSqlDatabase& database);

    QString m_databasePath;
    QString m_connectionName;
};

#endif // DESKTOP_PET_SQLITE_EVENT_REPOSITORY_H
