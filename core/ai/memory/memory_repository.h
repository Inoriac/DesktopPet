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

    virtual bool insert(const MemoryEntry& entry) = 0;
    virtual bool update(const MemoryEntry& entry) = 0;
    virtual bool updateStatus(const QString& id,
                              MemoryStatus status,
                              const QJsonObject& payloadPatch = {}) = 0;
    virtual QList<MemoryEntry> loadAll() = 0;
    virtual bool clear() = 0;
};

#endif // DESKTOP_PET_MEMORY_REPOSITORY_H
