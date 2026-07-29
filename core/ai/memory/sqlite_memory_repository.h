#ifndef DESKTOP_PET_SQLITE_MEMORY_REPOSITORY_H
#define DESKTOP_PET_SQLITE_MEMORY_REPOSITORY_H

#include "memory_repository.h"

#include <QSqlDatabase>

class SQLiteMemoryRepository : public MemoryRepository {
public:
    SQLiteMemoryRepository();
    ~SQLiteMemoryRepository() override;

    bool open(const QString& path, QString* errorMessage = nullptr) override;
    void close() override;
    bool isOpen() const override;
    QString connectionName() const override { return m_connectionName; }

    bool insert(const MemoryEntry& entry) override;
    bool update(const MemoryEntry& entry) override;
    bool updateStatus(const QString& id,
                      MemoryStatus status,
                      const QJsonObject& payloadPatch = {}) override;
    QList<MemoryEntry> loadAll() override;
    bool clear() override;
    bool removeById(const QString& id) override;

    bool beginTransaction() override;
    bool commitTransaction() override;
    bool rollbackTransaction() override;

private:
    bool initSchema(QString* errorMessage = nullptr);
    bool insertTags(const QString& memoryId, const QStringList& tags);
    bool insertEvidence(const QString& memoryId, const QStringList& evidence);
    void deleteTags(const QString& memoryId);
    void deleteEvidence(const QString& memoryId);
    QStringList loadTags(const QString& memoryId);
    QStringList loadEvidence(const QString& memoryId);

    QString m_connectionName;
};

#endif // DESKTOP_PET_SQLITE_MEMORY_REPOSITORY_H
