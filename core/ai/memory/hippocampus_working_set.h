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
    
    // Reload a bounded recency window; quality/emotion do not select its rows.
    bool refresh();
    
    const QList<MemoryEntry>& items() const { return m_items; }
    int size() const { return m_items.size(); }
    bool isEmpty() const { return m_items.isEmpty(); }
    
    // Rank the whole window by 0.8 * content relevance + 0.2 * recency,
    // then take the seed budget (not a full SQLite scan).
    QList<MemoryEntry> scan(const QString& queryText,
                            const QStringList& requiredTags = {},
                            int limit = 8) const;
    
    int totalPendingCount() const { return m_totalPendingCount; }
    
private:
    double computePriority(const MemoryEntry& entry, double relevance,
                           const QDateTime& now) const;
    
    MemoryStore* m_store = nullptr;
    int m_capacity = DEFAULT_CAPACITY;
    QList<MemoryEntry> m_items;
    int m_totalPendingCount = 0;
};

#endif // DESKTOP_PET_HIPPOCAMPUS_WORKING_SET_H
