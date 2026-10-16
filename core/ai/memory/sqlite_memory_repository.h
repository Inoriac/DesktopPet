#ifndef DESKTOP_PET_SQLITE_MEMORY_REPOSITORY_H
#define DESKTOP_PET_SQLITE_MEMORY_REPOSITORY_H

#include "memory_repository.h"

#include <QSqlDatabase>

class QSqlQuery;

class SQLiteMemoryRepository : public MemoryRepository {
public:
    SQLiteMemoryRepository();
    ~SQLiteMemoryRepository() override;

    bool open(const QString& path, QString* errorMessage = nullptr) override;
    void close() override;
    bool isOpen() const override;
    bool lastReadSucceeded() const override {
        return m_lastReadSucceeded;
    }
    QString connectionName() const override { return m_connectionName; }

    bool writeAttribution(const MemoryEntry& entry);
    bool insert(const MemoryEntry& entry) override;
    bool update(const MemoryEntry& entry) override;
    bool updateStatus(const QString& id,
                      MemoryStatus status,
                      const QJsonObject& payloadPatch = {}) override;
    QList<MemoryEntry> loadAll() override;
    std::optional<MemoryEntry> loadById(const QString& id) override;
    QList<MemoryEntry> loadRecent(int limit,
                                  const QString& partition = QString(),
                                  bool activeOnly = false, bool* ok = nullptr) override;
    bool saveActiveMemorySnapshot(const QList<ActiveMemoryItem>& items,
                                  const QDateTime& savedAt) override;
    ActiveMemorySnapshot loadActiveMemorySnapshot(int limit = ActiveMemoryPool::MAX_POOL_SIZE) override;
    bool clear() override;
    bool removeById(const QString& id) override;

    bool beginTransaction() override;
    bool commitTransaction() override;
    bool rollbackTransaction() override;

private:
    bool initSchema(QString* errorMessage = nullptr);
    QList<MemoryEntry> loadQuery(QSqlQuery& query);
    MemoryAttribution loadAttribution(const QString& memoryId);
    bool insertTags(const QString& memoryId, const QStringList& tags);
    bool insertEvidence(const QString& memoryId, const QStringList& evidence);
    bool deleteTags(const QString& memoryId);
    bool deleteEvidence(const QString& memoryId);
    QStringList loadTags(const QString& memoryId);
    QStringList loadEvidence(const QString& memoryId);

    bool m_lastReadSucceeded = true;
    QString m_connectionName;
    QStringList m_transactionSavepoints;
};

#endif // DESKTOP_PET_SQLITE_MEMORY_REPOSITORY_H
