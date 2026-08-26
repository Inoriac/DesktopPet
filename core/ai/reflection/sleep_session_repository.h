#ifndef DESKTOP_PET_SLEEP_SESSION_REPOSITORY_H
#define DESKTOP_PET_SLEEP_SESSION_REPOSITORY_H

#include <QSqlDatabase>

#include <optional>

#include "reflection_types.h"

class SleepSessionRepository {
public:
    SleepSessionRepository();
    ~SleepSessionRepository();

    Result<void, DomainError> open(const QString& databasePath);
    void close();
    bool isOpen() const;
    const QString& databasePath() const { return m_databasePath; }

    Result<void, DomainError> createPending(const SleepSessionRecord& session);
    Result<std::optional<SleepSessionRecord>, DomainError> find(
        const QString& sessionId) const;
    Result<QList<SleepSessionRecord>, DomainError> incomplete(
        const QString& profileId) const;
    Result<qint64, DomainError> latestEventSequence(
        const QString& profileId) const;
    Result<void, DomainError> updateState(
        const QString& sessionId,
        SleepSessionState state);
    Result<void, DomainError> decideCommit(const QString& sessionId);
    Result<void, DomainError> decideAbort(const QString& sessionId);
    Result<void, DomainError> markParticipantFinalized(
        const QString& sessionId,
        const QString& participant);
    Result<void, DomainError> markCompleted(const QString& sessionId);
    Result<void, DomainError> markRolledBack(const QString& sessionId);

private:
    Result<void, DomainError> migrateSchema(QSqlDatabase& database);

    QString m_databasePath;
    QString m_connectionName;
};

#endif // DESKTOP_PET_SLEEP_SESSION_REPOSITORY_H
