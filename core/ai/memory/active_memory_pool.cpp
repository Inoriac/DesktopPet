#include "active_memory_pool.h"

#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <limits>
#include <QRegularExpression>
#include <QUuid>
#include "recall_text.h"

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
    if (memoryId.trimmed().isEmpty() || !std::isfinite(activation)
        || activation <= 0.0) {
        return;
    }
    activation = std::min(activation, 2.0);
    
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
    m_contextTopics.clear();
    m_contextKey.clear();
    m_lastDecayAt = {};
    invalidateTopic();
}

QList<ActiveMemoryItem> ActiveMemoryPool::snapshot(int maxCount) const {
    if (maxCount <= 0) return {};
    QList<ActiveMemoryItem> items = activeItems();
    if (items.size() > maxCount) {
        items = items.mid(0, maxCount);
    }
    return items;
}

void ActiveMemoryPool::restoreFromSnapshot(const QList<ActiveMemoryItem>& items,
                                            const QDateTime& savedAt,
                                            const QDateTime& now) {
    // Snapshots are untrusted persisted state.  Ignore malformed rows and
    // enforce the same bounds as live activation so a corrupt snapshot cannot
    // grow the pool beyond its fixed budget.
    auto insertRestored = [this](const ActiveMemoryItem& item, double activation) {
        if (item.memoryId.trimmed().isEmpty()) return;
        if (!std::isfinite(activation) || activation < ACTIVATION_THRESHOLD) return;
        ActiveMemoryItem restored = item;
        restored.activation = std::min(activation, 2.0);
        if (restored.source.trimmed().isEmpty()) {
            restored.source = QStringLiteral("unknown");
        }
        const auto existing = m_pool.constFind(restored.memoryId);
        if (existing != m_pool.constEnd() && existing->activation >= restored.activation) {
            return;
        }
        m_pool.insert(restored.memoryId, restored);
        while (m_pool.size() > MAX_POOL_SIZE) {
            QString minKey;
            double minActivation = std::numeric_limits<double>::max();
            for (auto it = m_pool.constBegin(); it != m_pool.constEnd(); ++it) {
                if (it->activation < minActivation) {
                    minActivation = it->activation;
                    minKey = it.key();
                }
            }
            if (minKey.isEmpty()) break;
            m_pool.remove(minKey);
        }
    };

    if (!savedAt.isValid() || !now.isValid() || savedAt >= now) {
        // Invalid timestamps or negative time delta - skip decay
        for (const ActiveMemoryItem& item : items) {
            insertRestored(item, item.activation);
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
        
        insertRestored(item, restoredActivation);
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

namespace {
double topicCosine(const QVector<float>& a, const QVector<float>& b) {
    if (a.isEmpty() || a.size() != b.size()) return 0;
    double dot = 0, aa = 0, bb = 0;
    for (qsizetype i = 0; i < a.size(); ++i) { dot += double(a[i])*b[i]; aa += double(a[i])*a[i]; bb += double(b[i])*b[i]; }
    return aa > 0 && bb > 0 ? dot / std::sqrt(aa*bb) : 0;
}
QStringList numericCues(const QString& text) {
    static const QRegularExpression numbers(QStringLiteral("[0-9]+(?:[.,][0-9]+)*|[零〇一二两三四五六七八九十百千万亿]+"));
    QStringList out;
    auto matches = numbers.globalMatch(text);
    while (matches.hasNext()) out.append(matches.next().captured());
    return out;
}
}

bool ActiveMemoryPool::explicitFollowup(const QString& input) {
    static const QRegularExpression pattern(QStringLiteral(
        "^(那(这个|那个|第二个|第一个|它|具体)?呢[？?]?|继续[吧。]?|详细说说[。？?]?|为什么[？?]?|"
        "这件事呢[？?]?|tell me more[.!?]?|why[?]?|what about that[?]?)$"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(input.trimmed()).hasMatch();
}

QString ActiveMemoryPool::topicText() const {
    QStringList inputs;
    for (const auto& turn : m_topicTurns) inputs.append(turn.input);
    return inputs.join(QLatin1Char(' '));
}

TopicRecallDecision ActiveMemoryPool::assessTopic(
    const QString& input, const PreparedEmbeddingQuery& query, const QDateTime& now) const {
    TopicRecallDecision result;
    if (m_topicTurns.isEmpty()) return result;
    const auto& last = m_topicTurns.last();
    if (!now.isValid() || last.at.secsTo(now) < 0 || last.at.secsTo(now) >= TOPIC_IDLE_SECONDS) {
        result.reason = QStringLiteral("topic_idle"); return result;
    }
    if (query.model != last.query.model || query.vector.size() != last.query.vector.size()) {
        result.reason = QStringLiteral("embedding_changed"); return result;
    }
    if (query.attempted && !query.valid()) {
        result.reason = QStringLiteral("embedding_failed"); return result;
    }
    static const QRegularExpression switchTopic(QStringLiteral(
        "换个话题|换一个话题|另外问|另一个问题|顺便问|不聊|\\b(?:new topic|unrelated|switch topics)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression negative(QStringLiteral(
        "不|没|别|改为|改成|取消|而是|\\b(?:not|no|never|without|instead|change)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    if (switchTopic.match(input).hasMatch()) { result.reason = QStringLiteral("explicit_switch"); return result; }
    result.followup = explicitFollowup(input);
    if (result.followup) { result.related = result.reuse = true; result.reason = QStringLiteral("explicit_followup"); return result; }
    const auto currentTokens = RecallText::tokens(input);
    if (currentTokens.isEmpty()) { result.reason = QStringLiteral("uncertain_reference"); return result; }
    const auto historyTokens = RecallText::tokens(topicText());
    const QSet<QString> history(historyTokens.cbegin(), historyTokens.cend());
    const double coverage = RecallText::lexicalCoverage(input, currentTokens, history);
    double similarity = 0;
    for (const auto& turn : m_topicTurns)
        if (query.valid() && turn.query.valid()) similarity = std::max(similarity, topicCosine(query.vector, turn.query.vector));
    result.related = coverage >= 0.35 || similarity >= 0.75;
    if (negative.match(input).hasMatch()) { result.reason = QStringLiteral("changed_condition"); return result; }
    if (numericCues(input) != numericCues(last.input)) {
        result.reason = QStringLiteral("changed_number"); return result;
    }
    // Shared background alone (e.g. Hangzhou + a new hospital requirement)
    // cannot authorize reuse. Pure rewordings may use strong semantic evidence.
    if (coverage > 0.0 && coverage < TEXT_COVERAGE) {
        result.reason = QStringLiteral("new_cue"); return result;
    }
    result.reuse = coverage >= TEXT_COVERAGE || similarity >= TOPIC_SIMILARITY;
    result.reason = result.reuse ? QStringLiteral("topic_match") : QStringLiteral("uncertain_topic");
    return result;
}

QList<ActiveMemoryItem> ActiveMemoryPool::topicItems() const {
    QList<ActiveMemoryItem> result;
    for (const auto& item : m_topicItems) {
        if (!m_topicId.isEmpty() && item.contextId == m_topicId
            && item.topicTurn > 0 && item.topicTurn + TOPIC_TURN_WINDOW > m_topicSequence)
            result.append(item);
    }
    return result;
}

void ActiveMemoryPool::invalidateTopic() {
    m_topicItems.clear();
    m_topicTurns.clear();
    m_topicId.clear();
    m_topicSequence = 0;
}

void ActiveMemoryPool::recordRecall(const QString& input, const PreparedEmbeddingQuery& query,
                                     bool related, const QList<ActiveMemoryItem>& selected,
                                     const QDateTime& now) {
    if (!related || m_topicId.isEmpty()) {
        invalidateTopic();
        m_topicId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    ++m_topicSequence;
    m_topicTurns.append({input.left(2000), query, now});
    while (m_topicTurns.size() > TOPIC_TURN_WINDOW) m_topicTurns.removeFirst();
    for (auto item : selected) {
        if (item.memoryId.isEmpty() || !std::isfinite(item.activation)) continue;
        item.contextId = m_topicId;
        item.topicTurn = m_topicSequence;
        item.lastActivatedAt = now;
        item.source = QStringLiteral("session");
        // A retrieval is evidence for this turn, not a second activation event.
        m_pool.remove(item.memoryId);
        activate(item.memoryId, std::clamp(item.activation, ACTIVATION_THRESHOLD, 1.0), item.source, item.contextId);
        m_topicItems.erase(
            std::remove_if(m_topicItems.begin(), m_topicItems.end(),
                           [&](const auto& old) { return old.memoryId == item.memoryId; }),
            m_topicItems.end());
        m_topicItems.append(item);
        while (m_topicItems.size() > MAX_POOL_SIZE)
            m_topicItems.removeFirst();
        auto retained = m_pool.find(item.memoryId);
        if (retained != m_pool.end()) retained->topicTurn = item.topicTurn;
    }
}

void ActiveMemoryPool::selectContext(const QString& key) {
    if (key == m_contextKey)
        return;
    const auto now = QDateTime::currentDateTimeUtc();
    m_contextTopics[m_contextKey] = {m_topicTurns, m_topicId, m_topicSequence, m_topicItems, now};
    m_contextKey = key;
    const auto saved = m_contextTopics.take(key);
    m_topicTurns = saved.turns;
    m_topicId = saved.id;
    m_topicSequence = saved.sequence;
    m_topicItems = saved.items;
    while (m_contextTopics.size() >= 32) {
        auto oldest = m_contextTopics.begin();
        for (auto it = m_contextTopics.begin(); it != m_contextTopics.end(); ++it)
            if (it->used < oldest->used)
                oldest = it;
        m_contextTopics.erase(oldest);
    }
}
