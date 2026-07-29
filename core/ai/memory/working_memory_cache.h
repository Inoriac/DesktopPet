#ifndef DESKTOP_PET_WORKING_MEMORY_CACHE_H
#define DESKTOP_PET_WORKING_MEMORY_CACHE_H

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

struct WorkingMemoryItem {
    QString id;
    QString summary;
    QString content;
    QStringList tags;
    QString source;
    QDateTime createdAt;
    QDateTime expiresAt;
    double importance = 0.3;
    double emotionIntensity = 0.0;
    int mentionCount = 1;
};

class MemoryStore;

class WorkingMemoryCache {
public:
    void setCapacity(int maxItems);
    int capacity() const { return m_maxItems; }

    void add(const WorkingMemoryItem& item);
    QList<WorkingMemoryItem> all() const { return m_items; }
    int size() const { return m_items.size(); }

    void cleanup(MemoryStore* store = nullptr);
    void clear();

    static int defaultTtlSecs(const QString& source);

    // 累计 mentionCount：按 summary 命中所有缓存项的总提及次数。供 Daydream 写
    // ShortTerm 时持久化 recurrence 信号用（drain 据 mentionCount>=2 判升级）。
    int countMentions(const QString& summary) const;

private:
    void trimToCapacity();
    bool shouldConsolidate(const WorkingMemoryItem& item) const;
    void consolidateToStore(const WorkingMemoryItem& item, MemoryStore* store) const;

    QList<WorkingMemoryItem> m_items;
    int m_maxItems = 50;
};

#endif // DESKTOP_PET_WORKING_MEMORY_CACHE_H
