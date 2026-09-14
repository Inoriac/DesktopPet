#ifndef DESKTOP_PET_MEMORY_REPOSITORY_H
#define DESKTOP_PET_MEMORY_REPOSITORY_H

#include <QList>
#include <QString>

#include <algorithm>

#include "memory_types.h"
#include "active_memory_pool.h"

class MemoryRepository {
public:
    virtual ~MemoryRepository() = default;

    virtual bool open(const QString& path, QString* errorMessage = nullptr) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual QString connectionName() const = 0;

    virtual bool insert(const MemoryEntry& entry) = 0;
    virtual bool update(const MemoryEntry& entry) = 0;
    virtual bool updateStatus(const QString& id,
                              MemoryStatus status,
                              const QJsonObject& payloadPatch = {}) = 0;
    virtual QList<MemoryEntry> loadAll() = 0;

    // Bounded reads for latency-sensitive recall paths. Implementations should
    // apply the limit in the database query; the default preserves compatibility
    // for small in-memory repositories.
    virtual QList<MemoryEntry> loadRecent(int limit,
                                          const QString& partition = QString(),
                                          bool activeOnly = false) {
        QList<MemoryEntry> entries = loadAll();
        if (activeOnly) {
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                                         [](const MemoryEntry& entry) {
                                             return entry.status != MemoryStatus::Active;
                                         }),
                          entries.end());
        }
        if (limit > 0 && entries.size() > limit) entries = entries.mid(0, limit);
        return entries;
    }
    virtual bool clear() = 0;

    virtual bool saveActiveMemorySnapshot(const QList<ActiveMemoryItem>& items,
                                          const QDateTime& savedAt) {
        (void)items;
        (void)savedAt;
        return false;
    }
    virtual ActiveMemorySnapshot loadActiveMemorySnapshot(int limit = ActiveMemoryPool::MAX_POOL_SIZE) {
        (void)limit;
        return {};
    }

    // 物理删除单条记忆及其连带子表（tags/evidence/relations/embeddings/access_log）。
    // 供 Daydream 清空 Hippocampus 源条目用。同一事务内执行可随 ROLLBACK 撤销。
    virtual bool removeById(const QString& id) = 0;

    // 事务原子性，供 Daydream 整 session ROLLBACK 用。底层走同一命名连接的
    // QSqlDatabase::transaction/commit/rollback；同一连接上的插入会随 ROLLBACK 撤销。
    virtual bool beginTransaction() = 0;
    virtual bool commitTransaction() = 0;
    virtual bool rollbackTransaction() = 0;
};

#endif // DESKTOP_PET_MEMORY_REPOSITORY_H
