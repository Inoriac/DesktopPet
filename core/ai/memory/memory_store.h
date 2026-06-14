#ifndef DESKTOP_PET_MEMORY_STORE_H
#define DESKTOP_PET_MEMORY_STORE_H

#include <QList>
#include <QString>

#include "memory_types.h"

class MemoryStore {
public:
    void setStoragePath(const QString& memoryFilePath);
    const QString& storagePath() const { return m_memoryFilePath; }

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
    QString m_memoryFilePath = "log/ai_memory.json";
    QList<MemoryEntry> m_entries;
};

#endif // DESKTOP_PET_MEMORY_STORE_H