#ifndef DESKTOP_PET_MEMORY_STORE_H
#define DESKTOP_PET_MEMORY_STORE_H

#include <QList>
#include <QString>
#include <memory>

#include "memory_types.h"
#include "memory_relation_graph.h"

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
    bool updateEntryById(const MemoryEntry& entry);
    bool updateStatusById(const QString& id,
                          MemoryStatus status,
                          const QJsonObject& payloadPatch = {});
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

    MemoryRelationGraph& relationGraph() { return m_relationGraph; }
    const MemoryRelationGraph& relationGraph() const { return m_relationGraph; }

    // 底层 SQLite 连接名（与 SqliteMemoryRepository 共用），供需同库的组件复用
    // （如 SqliteEmbeddingIndex 写 memory_embeddings 表）。
    QString databaseConnectionName() const;

    MemoryEntry* findById(const QString& id);
    const MemoryEntry* findById(const QString& id) const;

private:
    void persistEntry(const MemoryEntry& entry);
    void persistStatusUpdate(const QString& id, MemoryStatus status, const QJsonObject& payloadPatch);

    QString m_memoryFilePath = QStringLiteral("log/ai_memory.json");
    QString m_databasePath = QStringLiteral("runtime/memory/memory.db");
    QList<MemoryEntry> m_entries;
    std::unique_ptr<MemoryRepository> m_repository;
    MemoryRelationGraph m_relationGraph;
};

#endif // DESKTOP_PET_MEMORY_STORE_H
