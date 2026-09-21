#include "memory_keyword_index.h"
#include "partition_policy.h"
#include "recall_text.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDebug>
#include <cmath>

namespace {
// Recheck eligibility at query time, including expiry while the cache is idle.
const QString eligible = QStringLiteral(
    "m.status='active' AND m.privacy_level!='sensitive' AND m.partition!='hippocampus' "
    "AND (m.expires_at IS NULL OR m.expires_at='' OR julianday(m.expires_at)>julianday('now'))");
}

void MemoryKeywordIndex::rebuild(const QList<MemoryEntry>& entries) {
    m_tokenPostings.clear(); m_tagPostings.clear();
    m_docTokens.clear(); m_docTags.clear(); m_indexedIds.clear();
    m_matcherDirty = true;
    for (const auto& entry : entries) upsert(entry);
}
void MemoryKeywordIndex::upsert(const MemoryEntry& entry) {
    if (!indexable(entry)) { remove(entry.id); return; }
    removeFromPostings(entry.id);
    m_docTokens[entry.id] = extractTokens(entry);
    for (const auto& token : m_docTokens[entry.id]) m_tokenPostings[token].insert(entry.id);
    QStringList tags;
    for (const auto& tag : entry.tags) {
        const auto key = RecallText::normalize(tag);
        if (!key.isEmpty() && !tags.contains(key)) tags.append(key);
    }
    m_docTags[entry.id] = tags;
    for (const auto& tag : tags) m_tagPostings[tag].insert(entry.id);
    m_indexedIds.insert(entry.id);
    m_matcherDirty = true;
}
void MemoryKeywordIndex::remove(const QString& id) {
    removeFromPostings(id);
    m_docTokens.remove(id); m_docTags.remove(id); m_indexedIds.remove(id);
    m_matcherDirty = true;
}
bool MemoryKeywordIndex::refreshGlobalTags(const QString& connectionName) {
    if (m_connectionName != connectionName) m_tagRevision = -1;
    m_connectionName = connectionName;
    if (connectionName.isEmpty() || !QSqlDatabase::contains(connectionName)) return false;
    QSqlQuery query(QSqlDatabase::database(connectionName, false));
    if (!query.exec(QStringLiteral("SELECT revision FROM memory_tag_catalog_state WHERE singleton=1"))
        || !query.next()) return false;
    const auto revision = query.value(0).toLongLong();
    query.finish();
    if (revision == m_tagRevision) return true;
    m_globalTags.clear();
    m_matcherDirty = true;
    if (!query.exec(QStringLiteral("SELECT DISTINCT t.normalized_tag FROM memory_tags t "
                                  "JOIN memory_items m ON m.id=t.memory_id WHERE ") + eligible)) return false;
    while (query.next()) {
        const auto tag = query.value(0).toString();
        if (!tag.isEmpty()) m_globalTags.insert(tag);
    }
    m_tagRevision = revision;
    return true;
}
QSet<QString> MemoryKeywordIndex::knownTags() const {
    QSet<QString> tags = m_globalTags;
    for (auto it = m_tagPostings.cbegin(); it != m_tagPostings.cend(); ++it) tags.insert(it.key());
    return tags;
}
void MemoryKeywordIndex::refreshMatcher() const {
    if (!m_matcherDirty) return;
    m_tagMatcher.setKnownTags(knownTags());
    m_matcherDirty = false;
}
MemoryCue MemoryKeywordIndex::extractCue(const QString& text) const {
    refreshMatcher();
    auto cue = m_tagMatcher.extractFromQuery(text);
    for (const auto& token : cue.tokens) {
        const double count = m_tokenPostings.value(token).size();
        cue.tokenWeights[token] = 1.0 + std::log((m_indexedIds.size() + 1.0) / (count + 1.0));
    }
    return cue;
}
QList<KeywordMatch> MemoryKeywordIndex::lookup(const MemoryCue& cue, int limit) const {
    if (limit <= 0) return {};
    QHash<QString, KeywordMatch> matches;
    QSet<QString> ids;
    for (const auto& token : cue.tokens) ids.unite(m_tokenPostings.value(RecallText::normalize(token)));
    // When SQLite is attached it is authoritative for tag eligibility.
    if (m_connectionName.isEmpty())
        for (const auto& tag : cue.knownTags) ids.unite(m_tagPostings.value(RecallText::normalize(tag)));
    for (const auto& id : ids) {
        const auto tokens = m_docTokens.value(id);
        KeywordMatch match;
        match.memoryId = id;
        match.lexicalScore = RecallText::lexicalCoverage(cue.normalizedQuery, cue.tokens,
            QSet<QString>(tokens.cbegin(), tokens.cend()), cue.tokenWeights);
        if (m_connectionName.isEmpty()) match.tagScore = RecallText::tagCoverage(cue.knownTags, m_docTags.value(id));
        matches.insert(id, match);
    }
    QStringList tags;
    for (const auto& raw : cue.knownTags) {
        const auto tag = RecallText::normalize(raw);
        if (!tag.isEmpty() && !tags.contains(tag)) tags.append(tag);
        if (tags.size() == 32) break;
    }
    if (!m_connectionName.isEmpty() && !tags.isEmpty() && QSqlDatabase::contains(m_connectionName)) {
        QStringList placeholders;
        for (int i = 0; i < tags.size(); ++i) placeholders.append(QStringLiteral(":tag%1").arg(i));
        QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
        query.prepare(QStringLiteral(
            "SELECT m.id, COUNT(DISTINCT t.normalized_tag) AS hits FROM memory_tags t "
            "JOIN memory_items m ON m.id=t.memory_id WHERE t.normalized_tag IN (")
            + placeholders.join(QLatin1Char(',')) + QStringLiteral(") AND ") + eligible
            + QStringLiteral(" GROUP BY m.id ORDER BY hits DESC, m.importance DESC, "
                             "COALESCE(m.updated_at,m.created_at) DESC, m.id LIMIT :limit"));
        for (int i = 0; i < tags.size(); ++i) query.bindValue(placeholders[i], tags[i]);
        query.bindValue(QStringLiteral(":limit"), limit);
        if (query.exec()) {
            while (query.next()) {
                const auto id = query.value(0).toString();
                auto& match = matches[id];
                match.memoryId = id;
                match.tagScore = query.value(1).toDouble() / tags.size();
            }
        } else qWarning() << "[Recall] tag lookup failed";
    }
    auto ranked = matches.values();
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        const double left = std::max(a.lexicalScore, a.tagScore);
        const double right = std::max(b.lexicalScore, b.tagScore);
        if (left != right) return left > right;
        if (a.tagScore != b.tagScore) return a.tagScore > b.tagScore;
        return a.memoryId < b.memoryId;
    });
    if (ranked.size() > limit) ranked = ranked.mid(0, limit);
    return ranked;
}
QList<QString> MemoryKeywordIndex::lookup(const QStringList& tokens, const QStringList& tags, int limit) const {
    MemoryCue cue;
    cue.tokens = tokens;
    cue.knownTags = tags;
    QList<QString> ids;
    for (const auto& match : lookup(cue, limit)) ids.append(match.memoryId);
    return ids;
}
bool MemoryKeywordIndex::indexable(const MemoryEntry& entry) {
    if (entry.status != MemoryStatus::Active || entry.privacyLevel == PrivacyLevel::Sensitive) return false;
    if (entry.expiresAt.isValid() && entry.expiresAt <= QDateTime::currentDateTimeUtc()) return false;
    return (entry.partition.isEmpty() ? partitionForType(entry.type) : partitionFromString(entry.partition))
        != MemoryPartition::Hippocampus;
}
QStringList MemoryKeywordIndex::extractTokens(const MemoryEntry& entry) {
    return RecallText::tokens(entry.key + QLatin1Char(' ') + entry.summary + QLatin1Char(' ')
                              + entry.content + QLatin1Char(' ') + entry.scope);
}
void MemoryKeywordIndex::removeFromPostings(const QString& id) {
    for (const auto& token : m_docTokens.value(id)) {
        m_tokenPostings[token].remove(id);
        if (m_tokenPostings[token].isEmpty()) m_tokenPostings.remove(token);
    }
    for (const auto& tag : m_docTags.value(id)) {
        m_tagPostings[tag].remove(id);
        if (m_tagPostings[tag].isEmpty()) m_tagPostings.remove(tag);
    }
}
