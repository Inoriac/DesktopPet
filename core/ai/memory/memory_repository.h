#ifndef DESKTOP_PET_MEMORY_REPOSITORY_H
#define DESKTOP_PET_MEMORY_REPOSITORY_H

#include <QList>
#include <QString>

#include "memory_types.h"

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
    virtual bool clear() = 0;

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
