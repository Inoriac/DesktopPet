#ifndef DESKTOP_PET_MEMORY_KEYWORD_INDEX_H
#define DESKTOP_PET_MEMORY_KEYWORD_INDEX_H

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "memory_types.h"

// 关键词/标签倒排索引（内存派生结构，可随时从 MemoryStore 重建）。
// 只索引 Active、非 Sensitive 的长期记忆（Hippocampus 分区除外）。
// 查询按命中 token/tag 数打分，返回 Top-N id，不做全库线性扫描。
class MemoryKeywordIndex {
public:
    void rebuild(const QList<MemoryEntry>& entries);
    void upsert(const MemoryEntry& entry);
    void remove(const QString& memoryId);

    // 返回按命中数排序的记忆 id（最多 limit 条）。
    // tokens 命中权重 1.0，tags 命中权重 2.0。
    QList<QString> lookup(const QStringList& tokens,
                          const QStringList& tags,
                          int limit = 12) const;

    QSet<QString> knownTags() const;
    int indexedCount() const { return m_indexedIds.size(); }
    bool isEmpty() const { return m_indexedIds.isEmpty(); }

private:
    static bool indexable(const MemoryEntry& entry);
    static QStringList extractTokens(const MemoryEntry& entry);

    void removeFromPostings(const QString& memoryId);

    QHash<QString, QSet<QString>> m_tokenPostings;  // token -> memoryIds
    QHash<QString, QSet<QString>> m_tagPostings;    // tag -> memoryIds
    QHash<QString, QStringList> m_docTokens;        // memoryId -> tokens（便于删除）
    QHash<QString, QStringList> m_docTags;          // memoryId -> tags
    QSet<QString> m_indexedIds;
};

#endif // DESKTOP_PET_MEMORY_KEYWORD_INDEX_H
