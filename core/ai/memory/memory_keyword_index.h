#ifndef DESKTOP_PET_MEMORY_KEYWORD_INDEX_H
#define DESKTOP_PET_MEMORY_KEYWORD_INDEX_H

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "memory_types.h"
#include "memory_cue_extractor.h"

struct KeywordMatch {
    QString memoryId;
    double lexicalScore = 0.0;
    double tagScore = 0.0;
};

// 关键词/标签倒排索引（内存派生结构，可随时从 MemoryStore 重建）。
// 只索引 Active、非 Sensitive 的长期记忆（Hippocampus 分区除外）。
// Text postings cover the working window; SQLite tag postings cover all history.
class MemoryKeywordIndex {
public:
    void rebuild(const QList<MemoryEntry>& entries);
    void upsert(const MemoryEntry& entry);
    void remove(const QString& memoryId);
    bool refreshGlobalTags(const QString& connectionName);
    MemoryCue extractCue(const QString& text) const;
    QList<KeywordMatch> lookup(const MemoryCue& cue, int limit = 12) const;

    // Compatibility wrapper; both signals use bounded coverage, not hit counts.
    QList<QString> lookup(const QStringList& tokens,
                          const QStringList& tags,
                          int limit = 12) const;

    QSet<QString> knownTags() const;
    int indexedCount() const { return m_indexedIds.size(); }
    bool isEmpty() const { return m_indexedIds.isEmpty() && m_globalTags.isEmpty(); }

private:
    static bool indexable(const MemoryEntry& entry);
    static QStringList extractTokens(const MemoryEntry& entry);

    void removeFromPostings(const QString& memoryId);
    void refreshMatcher() const;

    QHash<QString, QSet<QString>> m_tokenPostings;  // token -> memoryIds
    QHash<QString, QSet<QString>> m_tagPostings;    // tag -> memoryIds
    QHash<QString, QStringList> m_docTokens;        // memoryId -> tokens（便于删除）
    QHash<QString, QStringList> m_docTags;          // memoryId -> tags
    QSet<QString> m_indexedIds;
    QString m_connectionName;
    qint64 m_tagRevision = -1;
    QSet<QString> m_globalTags;
    mutable MemoryCueExtractor m_tagMatcher;
    mutable bool m_matcherDirty = true;
};

#endif // DESKTOP_PET_MEMORY_KEYWORD_INDEX_H
