#include "memory_keyword_index.h"

#include <algorithm>
#include <QRegularExpression>

#include "partition_policy.h"

void MemoryKeywordIndex::rebuild(const QList<MemoryEntry>& entries) {
    m_tokenPostings.clear();
    m_tagPostings.clear();
    m_docTokens.clear();
    m_docTags.clear();
    m_indexedIds.clear();
    
    for (const MemoryEntry& entry : entries) {
        if (indexable(entry)) {
            upsert(entry);
        }
    }
}

void MemoryKeywordIndex::upsert(const MemoryEntry& entry) {
    if (!indexable(entry)) {
        remove(entry.id);  // 清理可能存在的旧版本
        return;
    }
    
    // 先删除旧索引
    removeFromPostings(entry.id);
    
    // 提取 tokens
    const QStringList tokens = extractTokens(entry);
    m_docTokens[entry.id] = tokens;
    for (const QString& token : tokens) {
        m_tokenPostings[token].insert(entry.id);
    }
    
    // 索引 tags
    const QStringList tags = entry.tags;
    m_docTags[entry.id] = tags;
    for (const QString& tag : tags) {
        const QString normalized = tag.toLower().trimmed();
        if (!normalized.isEmpty()) {
            m_tagPostings[normalized].insert(entry.id);
        }
    }
    
    m_indexedIds.insert(entry.id);
}

void MemoryKeywordIndex::remove(const QString& memoryId) {
    if (!m_indexedIds.contains(memoryId)) return;
    
    removeFromPostings(memoryId);
    m_docTokens.remove(memoryId);
    m_docTags.remove(memoryId);
    m_indexedIds.remove(memoryId);
}

QList<QString> MemoryKeywordIndex::lookup(const QStringList& tokens,
                                          const QStringList& tags,
                                          int limit) const {
    QHash<QString, double> scores;
    
    // Token hits (weight 1.0)
    for (const QString& token : tokens) {
        if (m_tokenPostings.contains(token)) {
            for (const QString& memId : m_tokenPostings[token]) {
                scores[memId] += 1.0;
            }
        }
    }
    
    // Tag hits (weight 2.0)
    for (const QString& tag : tags) {
        const QString normalized = tag.toLower().trimmed();
        if (m_tagPostings.contains(normalized)) {
            for (const QString& memId : m_tagPostings[normalized]) {
                scores[memId] += 2.0;
            }
        }
    }
    
    // Sort by score
    QList<QPair<QString, double>> ranked;
    for (auto it = scores.constBegin(); it != scores.constEnd(); ++it) {
        ranked.append({it.key(), it.value()});
    }
    
    std::sort(ranked.begin(), ranked.end(),
        [](const QPair<QString, double>& a, const QPair<QString, double>& b) {
            return a.second > b.second;
        });
    
    // Extract top N ids
    QList<QString> results;
    const int count = std::min(limit, static_cast<int>(ranked.size()));
    for (int i = 0; i < count; ++i) {
        results.append(ranked[i].first);
    }
    
    return results;
}

QSet<QString> MemoryKeywordIndex::knownTags() const {
    QSet<QString> tags;
    for (auto it = m_tagPostings.constBegin(); it != m_tagPostings.constEnd(); ++it) {
        tags.insert(it.key());
    }
    return tags;
}

bool MemoryKeywordIndex::indexable(const MemoryEntry& entry) {
    // 只索引 Active、非 Sensitive、非 Hippocampus 的长期记忆
    if (entry.status != MemoryStatus::Active) return false;
    if (entry.privacyLevel == PrivacyLevel::Sensitive) return false;
    
    const MemoryPartition partition = entry.partition.isEmpty()
        ? partitionForType(entry.type)
        : partitionFromString(entry.partition);
    
    if (partition == MemoryPartition::Hippocampus) return false;
    
    return true;
}

QStringList MemoryKeywordIndex::extractTokens(const MemoryEntry& entry) {
    QString text = entry.key + QStringLiteral(" ")
                 + entry.summary + QStringLiteral(" ")
                 + entry.content + QStringLiteral(" ")
                 + entry.scope;
    
    // Normalize
    text = text.toLower().trimmed();
    
    // Extract Latin words (2+ chars)
    QStringList tokens;
    static const QRegularExpression latinRe(QStringLiteral("[a-z0-9]{2,}"));
    QRegularExpressionMatchIterator it = latinRe.globalMatch(text);
    while (it.hasNext()) {
        tokens.append(it.next().captured(0));
    }
    
    // Extract CJK bigrams
    QString cjkBuffer;
    for (const QChar& ch : text) {
        const ushort code = ch.unicode();
        if (code >= 0x4E00 && code <= 0x9FFF) {
            cjkBuffer.append(ch);
        } else {
            if (cjkBuffer.length() >= 2) {
                for (int i = 0; i + 2 <= cjkBuffer.length(); ++i) {
                    tokens.append(cjkBuffer.mid(i, 2));
                }
            }
            cjkBuffer.clear();
        }
    }
    
    if (cjkBuffer.length() >= 2) {
        for (int i = 0; i + 2 <= cjkBuffer.length(); ++i) {
            tokens.append(cjkBuffer.mid(i, 2));
        }
    }
    
    // Deduplicate
    QSet<QString> seen;
    QStringList unique;
    for (const QString& token : tokens) {
        if (!seen.contains(token)) {
            seen.insert(token);
            unique.append(token);
        }
    }
    
    return unique;
}

void MemoryKeywordIndex::removeFromPostings(const QString& memoryId) {
    // Remove from token postings
    if (m_docTokens.contains(memoryId)) {
        for (const QString& token : m_docTokens[memoryId]) {
            if (m_tokenPostings.contains(token)) {
                m_tokenPostings[token].remove(memoryId);
                if (m_tokenPostings[token].isEmpty()) {
                    m_tokenPostings.remove(token);
                }
            }
        }
    }
    
    // Remove from tag postings
    if (m_docTags.contains(memoryId)) {
        for (const QString& tag : m_docTags[memoryId]) {
            const QString normalized = tag.toLower().trimmed();
            if (m_tagPostings.contains(normalized)) {
                m_tagPostings[normalized].remove(memoryId);
                if (m_tagPostings[normalized].isEmpty()) {
                    m_tagPostings.remove(normalized);
                }
            }
        }
    }
}
