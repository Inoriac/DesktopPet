#ifndef DESKTOP_PET_MEMORY_STORE_H
#define DESKTOP_PET_MEMORY_STORE_H

#include <QList>
#include <QString>
#include <memory>

#include "memory_types.h"
#include "memory_relation_graph.h"
#include "tag_cooccurrence_graph.h"

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
    bool importLegacyJson(const QString& jsonPath,
                          QString* errorMessage = nullptr);

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
    // 物理删除单条（含子表）。事务内调用可随 ROLLBACK 撤销。Daydream 清空 inbox 用。
    bool removeEntryById(const QString& id);

    QList<MemoryEntry> all() const { return m_entries; }
    QList<MemoryEntry> recent(MemoryType type, int limit) const;
    QList<MemoryEntry> findByTag(const QString& tag, int limit = 20) const;
    QStringList summaryForContext(int limit = 8) const;
    void clear();

    MemoryRelationGraph& relationGraph() { return m_relationGraph; }
    const MemoryRelationGraph& relationGraph() const { return m_relationGraph; }
    TagCooccurrenceGraph& tagCooccurrenceGraph() { return m_tagCooccurrenceGraph; }
    const TagCooccurrenceGraph& tagCooccurrenceGraph() const { return m_tagCooccurrenceGraph; }

    // 底层 SQLite 连接名（与 SqliteMemoryRepository 共用），供需同库的组件复用
    // （如 SqliteEmbeddingIndex 写 memory_embeddings 表）。
    QString databaseConnectionName() const;

    // 事务原子性，供 Daydream 整 session ROLLBACK 用。beginafter 内所有写入（含
    // MemoryRelationGraph 等复用同一连接的组件）在 commit 前未落盘，rollback 全撤销。
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

    MemoryEntry* findById(const QString& id);
    const MemoryEntry* findById(const QString& id) const;

private:
    bool persistEntry(const MemoryEntry& entry);
    bool persistStatusUpdate(const QString& id, MemoryStatus status, const QJsonObject& payloadPatch);

    QString m_memoryFilePath = QStringLiteral("log/ai_memory.json");
    QString m_databasePath = QStringLiteral("runtime/memory/memory.db");
    QList<MemoryEntry> m_entries;
    std::unique_ptr<MemoryRepository> m_repository;
    MemoryRelationGraph m_relationGraph;
    TagCooccurrenceGraph m_tagCooccurrenceGraph;
};

#endif // DESKTOP_PET_MEMORY_STORE_H
