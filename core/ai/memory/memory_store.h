#ifndef DESKTOP_PET_MEMORY_STORE_H
#define DESKTOP_PET_MEMORY_STORE_H

#include <QList>
#include <QString>
#include <memory>

#include "memory_types.h"

class MemoryRepository;

class MemoryStore {
public:
    MemoryStore();
    ~MemoryStore();

    void setStoragePath(const QString& memoryFilePath);
    const QString& storagePath() const { return m_memoryFilePath; }

    void setDatabasePath(const QString& dbPath);
    const QString& databasePath() const { return m_databasePath; }

    bool load(QString* errorMessage = nullptr);
    bool save(QString* errorMessage = nullptr) const;

    MemoryEntry add(MemoryType type,
                    const QString& key,
                    const QJsonValue& value,
                    const QStringList& tags = {});
    MemoryEntry addEntry(const MemoryEntry& entry);
    bool updateStatusByKey(MemoryType type,
                           const QString& key,
                           MemoryStatus status,
                           const QJsonObject& payloadPatch = {});
    bool updateTaskShadowStatus(const QString& linkedTaskId,
                                MemoryStatus status,
                                const QJsonObject& payloadPatch = {});

    QList<MemoryEntry> all() const { return m_entries; }
    QList<MemoryEntry> recent(MemoryType type, int limit) const;
    QList<MemoryEntry> findByTag(const QString& tag, int limit = 20) const;
    QStringList summaryForContext(int limit = 8) const;
    void clear();

private:
    void persistEntry(const MemoryEntry& entry);
    void persistStatusUpdate(const QString& id, MemoryStatus status, const QJsonObject& payloadPatch);

    QString m_memoryFilePath = QStringLiteral("log/ai_memory.json");
    QString m_databasePath = QStringLiteral("runtime/memory/memory.db");
    QList<MemoryEntry> m_entries;
    std::unique_ptr<MemoryRepository> m_repository;
};

#endif // DESKTOP_PET_MEMORY_STORE_H
