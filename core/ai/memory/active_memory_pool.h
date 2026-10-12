#ifndef DESKTOP_PET_ACTIVE_MEMORY_POOL_H
#define DESKTOP_PET_ACTIVE_MEMORY_POOL_H

#include <QDateTime>
#include <QHash>
#include <QString>
#include "embedding_index.h"

struct ActiveMemoryItem {
    QString memoryId;
    double activation = 0.0;
    QString source;  // "session", "association", "recent", "emotion", "goal"
    QDateTime lastActivatedAt;
    QString contextId;
    quint64 topicTurn = 0; // Runtime only; never restored as dialogue context.
};

struct ActiveMemorySnapshot {
    QList<ActiveMemoryItem> items;
    QDateTime savedAt;
    bool isEmpty() const { return items.isEmpty(); }
};

struct TopicRecallDecision {
    bool related = false;
    bool reuse = false;
    bool followup = false;
    QString reason = QStringLiteral("no_topic");
};

class ActiveMemoryPool {
public:
    static constexpr int MAX_POOL_SIZE = 50;
    static constexpr double ACTIVATION_THRESHOLD = 0.05;
    static constexpr int TOPIC_TURN_WINDOW = 6;
    static constexpr int TOPIC_IDLE_SECONDS = 30 * 60;
    // Conservative provisional defaults; calibrate on Windows with the real model.
    static constexpr double TOPIC_SIMILARITY = 0.86;
    static constexpr double CANDIDATE_SIMILARITY = 0.84;
    static constexpr double TEXT_COVERAGE = 0.75;
    static bool explicitFollowup(const QString& input);
    TopicRecallDecision assessTopic(const QString& input, const PreparedEmbeddingQuery& query,
                                    const QDateTime& now = QDateTime::currentDateTimeUtc()) const;
    QList<ActiveMemoryItem> topicItems() const;
    void recordRecall(const QString& input, const PreparedEmbeddingQuery& query,
                      bool related, const QList<ActiveMemoryItem>& selected,
                      const QDateTime& now = QDateTime::currentDateTimeUtc());
    void invalidateTopic();
    QString topicText() const;


    void activate(const QString& memoryId,
                  double activation,
                  const QString& source,
                  const QString& contextId = QString());
    
    void decay(double elapsedMinutes);
    
    // 基于墙钟的惰性衰减：计算距上次衰减的分钟数并执行 decay。
    // 召回流水线入口处调用（读取激活池之前），同一轮不重复衰减。
    void decayToNow(const QDateTime& now = QDateTime::currentDateTimeUtc());
    
    QList<ActiveMemoryItem> activeItems() const;
    QList<QString> activeMemoryIds(double minActivation = 0.0) const;
    
    bool contains(const QString& memoryId) const;
    double getActivation(const QString& memoryId) const;
    
    void clear();
    
    // Save/restore for offline recovery
    QList<ActiveMemoryItem> snapshot(int maxCount = MAX_POOL_SIZE) const;
    void restoreFromSnapshot(const QList<ActiveMemoryItem>& items,
                             const QDateTime& savedAt,
                             const QDateTime& now);

private:
    static double halfLifeMinutes(const QString& source);
    
    QHash<QString, ActiveMemoryItem> m_pool;
    QDateTime m_lastDecayAt;
    struct TopicTurn { QString input; PreparedEmbeddingQuery query; QDateTime at; };
    QList<TopicTurn> m_topicTurns;
    QString m_topicId;
    quint64 m_topicSequence = 0;
};

#endif // DESKTOP_PET_ACTIVE_MEMORY_POOL_H
