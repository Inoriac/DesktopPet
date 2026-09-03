#ifndef DESKTOP_PET_HIPPOCAMPUS_WORKING_SET_H
#define DESKTOP_PET_HIPPOCAMPUS_WORKING_SET_H

#include <QDateTime>
#include <QList>
#include <QString>

#include "memory_types.h"

class MemoryStore;

class HippocampusWorkingSet {
public:
    static constexpr int DEFAULT_CAPACITY = 200;
    
    explicit HippocampusWorkingSet(MemoryStore* store = nullptr);
    
    void setStore(MemoryStore* store);
    void setCapacity(int capacity);
    int capacity() const { return m_capacity; }
    
    // Reload working set from Hippocampus partition (most recent/high-priority)
    bool refresh();
    
    const QList<MemoryEntry>& items() const { return m_items; }
    int size() const { return m_items.size(); }
    bool isEmpty() const { return m_items.isEmpty(); }
    
    // Linear scan over working set (not full SQLite)
    QList<MemoryEntry> scan(const QString& queryText,
                            const QStringList& requiredTags = {},
                            int limit = 8) const;
    
    int totalPendingCount() const { return m_totalPendingCount; }
    
private:
    double computePriority(const MemoryEntry& entry, const QDateTime& now) const;
    
    MemoryStore* m_store = nullptr;
    int m_capacity = DEFAULT_CAPACITY;
    QList<MemoryEntry> m_items;
    int m_totalPendingCount = 0;
};

#endif // DESKTOP_PET_HIPPOCAMPUS_WORKING_SET_H
