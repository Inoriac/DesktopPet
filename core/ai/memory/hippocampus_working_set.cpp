#include "hippocampus_working_set.h"

#include <algorithm>

#include <QDateTime>

#include "memory_store.h"
#include "partition_policy.h"

HippocampusWorkingSet::HippocampusWorkingSet(MemoryStore* store)
    : m_store(store) {}

void HippocampusWorkingSet::setStore(MemoryStore* store) {
    m_store = store;
}

void HippocampusWorkingSet::setCapacity(int capacity) {
    m_capacity = capacity > 0 ? capacity : DEFAULT_CAPACITY;
}

bool HippocampusWorkingSet::refresh() {
    m_items.clear();
    m_totalPendingCount = 0;
    
    if (!m_store) return false;
    
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QList<MemoryEntry> candidates;
    
    // Load all Hippocampus partition entries
    for (const MemoryEntry& entry : m_store->all()) {
        if (entry.partition != QLatin1String("hippocampus")) continue;
        if (entry.status != MemoryStatus::Active) continue;
        
        candidates.append(entry);
    }
    
    m_totalPendingCount = candidates.size();
    
    // Sort by priority: recent + importance + emotion intensity
    std::sort(candidates.begin(), candidates.end(),
        [this, &now](const MemoryEntry& a, const MemoryEntry& b) {
            return computePriority(a, now) > computePriority(b, now);
        });
    
    // Take top N
    const int limit = std::min(m_capacity, static_cast<int>(candidates.size()));
    for (int i = 0; i < limit; ++i) {
        m_items.append(candidates[i]);
    }
    
    return true;
}

QList<MemoryEntry> HippocampusWorkingSet::scan(const QString& queryText,
                                                const QStringList& requiredTags,
                                                int limit) const {
    const QString normalizedQuery = queryText.trimmed().toLower();
    QList<MemoryEntry> results;
    
    for (const MemoryEntry& entry : m_items) {
        // Tag filter
        if (!requiredTags.isEmpty()) {
            bool hasAllTags = true;
            for (const QString& tag : requiredTags) {
                if (!entry.tags.contains(tag, Qt::CaseInsensitive)) {
                    hasAllTags = false;
                    break;
                }
            }
            if (!hasAllTags) continue;
        }
        
        // Text match (simple contains)
        if (!normalizedQuery.isEmpty()) {
            const QString searchSpace = (entry.key + QLatin1Char(' ')
                                        + entry.summary + QLatin1Char(' ')
                                        + entry.content + QLatin1Char(' ')
                                        + entry.tags.join(QLatin1Char(' '))).toLower();
            if (!searchSpace.contains(normalizedQuery)) {
                continue;
            }
        }
        
        results.append(entry);
        
        if (results.size() >= limit) break;
    }
    
    return results;
}

double HippocampusWorkingSet::computePriority(const MemoryEntry& entry,
                                               const QDateTime& now) const {
    double priority = 0.0;
    
    // Recency (within last 24 hours gets boost)
    if (entry.createdAt.isValid() && now.isValid()) {
        const qint64 ageSeconds = entry.createdAt.secsTo(now);
        const double ageHours = ageSeconds / 3600.0;
        if (ageHours < 24.0) {
            priority += (24.0 - ageHours) / 24.0;  // 0 to 1
        }
    }
    
    // Importance
    priority += entry.importance / 10.0;  // 0 to 1
    
    // Emotion intensity
    priority += entry.emotionIntensity * 0.5;  // 0 to 0.5
    
    // Mention count (cap at 5)
    priority += std::min(entry.mentionCount, 5) * 0.1;  // 0 to 0.5
    
    return priority;
}
