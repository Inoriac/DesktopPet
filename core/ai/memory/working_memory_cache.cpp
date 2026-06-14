#include "working_memory_cache.h"

#include <QUuid>

#include "memory_store.h"

void WorkingMemoryCache::setCapacity(int maxItems) {
    m_maxItems = qMax(1, maxItems);
}

void WorkingMemoryCache::add(const WorkingMemoryItem& item) {
    WorkingMemoryItem stored = item;
    if (stored.id.isEmpty()) {
        stored.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!stored.createdAt.isValid()) {
        stored.createdAt = QDateTime::currentDateTimeUtc();
    }
    if (!stored.expiresAt.isValid()) {
        const int ttl = defaultTtlSecs(stored.source);
        stored.expiresAt = stored.createdAt.addSecs(ttl);
    }

    for (WorkingMemoryItem& existing : m_items) {
        if (!existing.summary.isEmpty()
            && existing.summary.compare(stored.summary, Qt::CaseInsensitive) == 0) {
            existing.mentionCount += 1;
            existing.expiresAt = stored.expiresAt;
            if (stored.importance > existing.importance) {
                existing.importance = stored.importance;
            }
            return;
        }
    }

    m_items.append(stored);
    trimToCapacity();
}

void WorkingMemoryCache::cleanup(MemoryStore* store) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QList<WorkingMemoryItem> surviving;

    for (const WorkingMemoryItem& item : m_items) {
        if (item.expiresAt.isValid() && item.expiresAt <= now) {
            if (store && shouldConsolidate(item)) {
                consolidateToStore(item, store);
            }
            continue;
        }
        surviving.append(item);
    }

    m_items = surviving;
}

void WorkingMemoryCache::clear() {
    m_items.clear();
}

int WorkingMemoryCache::defaultTtlSecs(const QString& source) {
    if (source == QLatin1String("user_task"))         return 30 * 60;
    if (source == QLatin1String("tool_result"))       return 15 * 60;
    if (source == QLatin1String("topic"))             return 60 * 60;
    if (source == QLatin1String("assistant_response")) return 20 * 60;
    return 20 * 60;
}

void WorkingMemoryCache::trimToCapacity() {
    while (m_items.size() > m_maxItems) {
        int lowestIdx = 0;
        double lowestScore = m_items.first().importance;
        for (int i = 1; i < m_items.size(); ++i) {
            if (m_items[i].importance < lowestScore) {
                lowestScore = m_items[i].importance;
                lowestIdx = i;
            }
        }
        m_items.removeAt(lowestIdx);
    }
}

bool WorkingMemoryCache::shouldConsolidate(const WorkingMemoryItem& item) const {
    if (item.mentionCount >= 2) return true;
    if (item.emotionIntensity >= 0.7) return true;
    return false;
}

void WorkingMemoryCache::consolidateToStore(const WorkingMemoryItem& item, MemoryStore* store) const {
    if (!store) return;

    MemoryEntry entry;
    entry.type = MemoryType::Episodic;
    entry.status = MemoryStatus::Active;
    entry.privacyLevel = PrivacyLevel::Personal;
    entry.key = QStringLiteral("consolidated:%1").arg(item.id.left(8));
    entry.summary = item.summary;
    entry.content = item.content;
    entry.tags = item.tags;
    entry.source = QStringLiteral("consolidation");
    entry.importance = qMin(1.0, item.importance + 0.1);
    entry.strength = entry.importance;
    entry.emotionIntensity = item.emotionIntensity;

    if (item.mentionCount >= 2) {
        entry.confidence = 0.75;
    } else if (item.emotionIntensity >= 0.7) {
        entry.confidence = 0.7;
    } else {
        entry.confidence = 0.65;
    }

    store->addEntry(entry);
}

int WorkingMemoryCache::countMentions(const QString& summary) const {
    int count = 0;
    for (const WorkingMemoryItem& item : m_items) {
        if (item.summary.compare(summary, Qt::CaseInsensitive) == 0) {
            count += item.mentionCount;
        }
    }
    return count;
}
