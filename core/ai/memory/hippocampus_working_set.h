#ifndef DESKTOP_PET_HIPPOCAMPUS_WORKING_SET_H
#define DESKTOP_PET_HIPPOCAMPUS_WORKING_SET_H

#include <QDateTime>
#include <QList>
#include <QString>

#include "memory_types.h"

class MemoryStore;

struct HippocampusCandidate {
    MemoryEntry entry;
    double relevance = 0.0;
    double recency = 0.0;
    double score = 0.0;
};

class HippocampusWorkingSet {
public:
    static constexpr int DEFAULT_CAPACITY = 200;
    static constexpr qint64 DEFAULT_FRESHNESS_SECONDS = 7 * 24 * 60 * 60;
    
    explicit HippocampusWorkingSet(MemoryStore* store = nullptr);
    
    void setStore(MemoryStore* store);
    void setCapacity(int capacity);
    int capacity() const { return m_capacity; }
    void setFreshnessHorizonSeconds(qint64 seconds);
    double freshness(const MemoryEntry& entry, const QDateTime& now) const;
    
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
    
    QList<HippocampusCandidate> scanScored(const QString& queryText,
                                          const QStringList& requiredTags = {},
                                          int limit = 8) const;

    int totalPendingCount() const { return m_totalPendingCount; }
    
private:
    MemoryStore* m_store = nullptr;
    int m_capacity = DEFAULT_CAPACITY;
    qint64 m_freshnessSeconds = DEFAULT_FRESHNESS_SECONDS;
    QList<MemoryEntry> m_items;
    int m_totalPendingCount = 0;
};

#endif // DESKTOP_PET_HIPPOCAMPUS_WORKING_SET_H
