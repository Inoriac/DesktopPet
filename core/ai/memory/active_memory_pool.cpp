#include "active_memory_pool.h"

#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

double computeDecay(double elapsedMinutes, double halfLifeMinutes) {
    if (halfLifeMinutes <= 0.0 || elapsedMinutes <= 0.0) return 1.0;
    return std::pow(0.5, elapsedMinutes / halfLifeMinutes);
}

}

void ActiveMemoryPool::activate(const QString& memoryId,
                                 double activation,
                                 const QString& source,
                                 const QString& contextId) {
    if (memoryId.isEmpty()) return;
    
    const QDateTime now = QDateTime::currentDateTimeUtc();
    
    if (m_pool.contains(memoryId)) {
        ActiveMemoryItem& item = m_pool[memoryId];
        // Accumulate activation (max = 2.0 to prevent unbounded growth)
        item.activation = std::min(item.activation + activation, 2.0);
        item.lastActivatedAt = now;
        if (!source.isEmpty()) item.source = source;
        if (!contextId.isEmpty()) item.contextId = contextId;
    } else {
        ActiveMemoryItem item;
        item.memoryId = memoryId;
        item.activation = activation;
        item.source = source.isEmpty() ? QStringLiteral("unknown") : source;
        item.lastActivatedAt = now;
        item.contextId = contextId;
        m_pool.insert(memoryId, item);
    }
    
    // Prune if pool exceeds max size
    while (m_pool.size() > MAX_POOL_SIZE) {
        QString minKey;
        double minActivation = std::numeric_limits<double>::max();
        
        for (auto it = m_pool.constBegin(); it != m_pool.constEnd(); ++it) {
            if (it->activation < minActivation) {
                minActivation = it->activation;
                minKey = it.key();
            }
        }
        
        if (!minKey.isEmpty()) {
            m_pool.remove(minKey);
        } else {
            break;
        }
    }
}

void ActiveMemoryPool::decay(double elapsedMinutes) {
    if (elapsedMinutes <= 0.0) return;
    
    QList<QString> toRemove;
    
    for (auto it = m_pool.begin(); it != m_pool.end(); ++it) {
        const double halfLife = halfLifeMinutes(it->source);
        const double decayFactor = computeDecay(elapsedMinutes, halfLife);
        it->activation *= decayFactor;
        
        if (it->activation < ACTIVATION_THRESHOLD) {
            toRemove.append(it.key());
        }
    }
    
    for (const QString& id : toRemove) {
        m_pool.remove(id);
    }
}

void ActiveMemoryPool::decayToNow(const QDateTime& now) {
    if (!now.isValid()) return;
    
    if (!m_lastDecayAt.isValid()) {
        // 首次调用：只记录基准点，不衰减（池内项刚刚激活）
        m_lastDecayAt = now;
        return;
    }
    
    const qint64 elapsedSecs = m_lastDecayAt.secsTo(now);
    if (elapsedSecs < 60) return;  // 不足 1 分钟不衰减，避免频繁微小更新
    
    decay(elapsedSecs / 60.0);
    m_lastDecayAt = now;
}

QList<ActiveMemoryItem> ActiveMemoryPool::activeItems() const {
    QList<ActiveMemoryItem> items = m_pool.values();
    std::sort(items.begin(), items.end(),
        [](const ActiveMemoryItem& a, const ActiveMemoryItem& b) {
            return a.activation > b.activation;
        });
    return items;
}

QList<QString> ActiveMemoryPool::activeMemoryIds(double minActivation) const {
    QList<QString> ids;
    for (const ActiveMemoryItem& item : m_pool.values()) {
        if (item.activation >= minActivation) {
            ids.append(item.memoryId);
        }
    }
    return ids;
}

bool ActiveMemoryPool::contains(const QString& memoryId) const {
    return m_pool.contains(memoryId);
}

double ActiveMemoryPool::getActivation(const QString& memoryId) const {
    if (!m_pool.contains(memoryId)) return 0.0;
    return m_pool[memoryId].activation;
}

void ActiveMemoryPool::clear() {
    m_pool.clear();
}

QList<ActiveMemoryItem> ActiveMemoryPool::snapshot(int maxCount) const {
    QList<ActiveMemoryItem> items = activeItems();
    if (items.size() > maxCount) {
        items = items.mid(0, maxCount);
    }
    return items;
}

void ActiveMemoryPool::restoreFromSnapshot(const QList<ActiveMemoryItem>& items,
                                            const QDateTime& savedAt,
                                            const QDateTime& now) {
    if (!savedAt.isValid() || !now.isValid() || savedAt >= now) {
        // Invalid timestamps or negative time delta - skip decay
        for (const ActiveMemoryItem& item : items) {
            if (item.activation >= ACTIVATION_THRESHOLD) {
                m_pool.insert(item.memoryId, item);
            }
        }
        return;
    }
    
    const qint64 offlineSeconds = savedAt.secsTo(now);
    if (offlineSeconds < 0 || offlineSeconds > 86400 * 30) {
        // Negative or absurdly large duration (> 30 days) - discard瞬时激活
        return;
    }
    
    const double offlineMinutes = offlineSeconds / 60.0;
    
    for (const ActiveMemoryItem& item : items) {
        const double halfLife = halfLifeMinutes(item.source);
        const double decayFactor = computeDecay(offlineMinutes, halfLife);
        const double restoredActivation = item.activation * decayFactor;
        
        if (restoredActivation >= ACTIVATION_THRESHOLD) {
            ActiveMemoryItem restored = item;
            restored.activation = restoredActivation;
            m_pool.insert(restored.memoryId, restored);
        }
    }
}

double ActiveMemoryPool::halfLifeMinutes(const QString& source) {
    // Design §8 half-life policy
    if (source == QLatin1String("session")) return 60.0;      // 60 min
    if (source == QLatin1String("association")) return 30.0;  // 30 min
    if (source == QLatin1String("recent")) return 360.0;      // 6 hours
    if (source == QLatin1String("emotion")) return 360.0;     // 6 hours
    if (source == QLatin1String("goal")) return 1440.0;       // 24 hours
    return 60.0;  // Default fallback
}
