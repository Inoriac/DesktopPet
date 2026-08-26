#ifndef DESKTOP_PET_SQLITE_PRIVATE_PSYCHE_REPOSITORY_H
#define DESKTOP_PET_SQLITE_PRIVATE_PSYCHE_REPOSITORY_H

#include <QSqlDatabase>

#include <optional>

#include "ai/domain/domain_result.h"
#include "reflection_types.h"

class SqlitePrivatePsycheRepository {
public:
    SqlitePrivatePsycheRepository();
    ~SqlitePrivatePsycheRepository();

    Result<void, DomainError> open(const QString& databasePath);
    void close();
    bool isOpen() const;
    const QString& connectionName() const { return m_connectionName; }
    const QString& databasePath() const { return m_databasePath; }

    Result<void, DomainError> saveInnerThought(
        const InnerThoughtSummary& summary,
        const EncryptedPrivatePayload& encrypted,
        const QJsonObject& index);
    Result<std::optional<StoredPrivateRecord>, DomainError> innerThought(
        const QString& entryId) const;

    Result<void, DomainError> stageDiary(
        const QString& sessionId,
        const DiaryEntry& entry,
        const EncryptedPrivatePayload& encrypted);
    Result<QList<StoredPrivateRecord>, DomainError> preparedDiaries(
        const QString& sessionId,
        const QString& profileId) const;
    Result<QString, DomainError> finalizeDiary(
        const QString& sessionId,
        const DiaryEntry& entry,
        const EncryptedPrivatePayload& encrypted);
    Result<std::optional<StoredPrivateRecord>, DomainError> committedDiaryForDate(
        const QString& profileId, const QDate& localDate) const;
    Result<std::optional<StoredPrivateRecord>, DomainError> diary(
        const QString& entryId) const;

    Result<void, DomainError> abortSession(const QString& sessionId);

    int innerThoughtCount(const QString& profileId) const;
    int diaryCount(const QString& profileId) const;
    int preparedDiaryCount(const QString& sessionId) const;
    QByteArray stagedDiaryCiphertext(const QString& sessionId) const;

private:
    Result<void, DomainError> migrateSchema(QSqlDatabase& database);
    Result<std::optional<StoredPrivateRecord>, DomainError> readRecord(
        const QString& table,
        const QString& idColumn,
        const QString& recordType,
        const QString& recordId) const;

    QString m_databasePath;
    QString m_connectionName;
};

#endif // DESKTOP_PET_SQLITE_PRIVATE_PSYCHE_REPOSITORY_H
