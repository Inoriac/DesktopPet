#include "hippocampus_working_set.h"

#include <algorithm>

#include <QDateTime>

#include "memory_store.h"
#include "partition_policy.h"
#include "recall_text.h"

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
    
    // Read only the bounded, newest Hippocampus inbox rows from SQLite. The
    // worker and long-running recall paths must not rescan the full history.
    QList<MemoryEntry> candidates = m_store->loadRecentFromDatabase(
        m_capacity, QStringLiteral("hippocampus"), true);
    if (!m_store->databaseIsOpen()) {
        // An unopened in-memory store has no SQLite rows to query.
        for (const MemoryEntry& entry : m_store->all()) {
            if (entry.partition != QLatin1String("hippocampus")) continue;
            if (entry.status != MemoryStatus::Active) continue;
            candidates.append(entry);
        }
    } else {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [](const MemoryEntry& entry) {
                                            return entry.status != MemoryStatus::Active;
                                        }),
                         candidates.end());
    }
    
    m_totalPendingCount = candidates.size();
    
    // This query-independent cache is only a recency window. Do not let
    // importance, mentions or emotion remove candidates before a query exists.
    std::sort(candidates.begin(), candidates.end(),
        [](const MemoryEntry& a, const MemoryEntry& b) {
            const auto left = a.updatedAt.isValid() ? a.updatedAt : a.createdAt;
            const auto right = b.updatedAt.isValid() ? b.updatedAt : b.createdAt;
            return left != right ? left > right : a.id > b.id;
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
    if (limit <= 0) return {};
    const QString normalizedQuery = RecallText::normalize(queryText);
    const QStringList queryTokens = RecallText::tokens(normalizedQuery);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    struct ScoredEntry { MemoryEntry entry; double priority; };
    QList<ScoredEntry> candidates;
    
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
        
        double relevance = 0.0;
        if (!normalizedQuery.isEmpty()) {
            const QString searchSpace = RecallText::normalize(entry.key + QLatin1Char(' ')
                                        + entry.summary + QLatin1Char(' ')
                                        + entry.content + QLatin1Char(' ')
                                        + entry.scope);
            const auto terms = RecallText::tokens(searchSpace);
            relevance = RecallText::lexicalCoverage(normalizedQuery, queryTokens,
                QSet<QString>(terms.cbegin(), terms.cend()));
            // Preserve literal queries such as C++ or a single CJK character,
            // which do not necessarily produce lexical tokens.
            const int position = searchSpace.indexOf(normalizedQuery);
            if (position >= 0 && RecallText::boundaries(searchSpace, position, normalizedQuery.size())) {
                relevance = 1.0;
            }
            // A complete tag can match; tag fragments are not content evidence.
            for (const auto& tag : entry.tags)
                if (RecallText::normalize(tag) == normalizedQuery) relevance = 1.0;
            if (relevance <= 0.0) continue;
        }

        candidates.append({entry, computePriority(entry, relevance, now)});
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.entry.id < b.entry.id;
    });
    QList<MemoryEntry> results;
    for (int i = 0; i < std::min(limit, int(candidates.size())); ++i)
        results.append(candidates[i].entry);
    return results;
}

double HippocampusWorkingSet::computePriority(const MemoryEntry& entry,
                                               double relevance,
                                               const QDateTime& now) const {
    double recency = 0.0;
    // Keep the 24-hour creation-time window, bounded even for future dates.
    if (entry.createdAt.isValid() && now.isValid()) {
        recency = std::clamp(1.0 - entry.createdAt.secsTo(now) / 86400.0, 0.0, 1.0);
    }
    return 0.8 * std::clamp(relevance, 0.0, 1.0) + 0.2 * recency;
}
