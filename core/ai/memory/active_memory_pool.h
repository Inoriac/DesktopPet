#ifndef DESKTOP_PET_ACTIVE_MEMORY_POOL_H
#define DESKTOP_PET_ACTIVE_MEMORY_POOL_H

#include <QDateTime>
#include <QHash>
#include <QString>

struct ActiveMemoryItem {
    QString memoryId;
    double activation = 0.0;
    QString source;  // "session", "association", "recent", "emotion", "goal"
    QDateTime lastActivatedAt;
    QString contextId;
};

class ActiveMemoryPool {
public:
    static constexpr int MAX_POOL_SIZE = 50;
    static constexpr double ACTIVATION_THRESHOLD = 0.05;

    void activate(const QString& memoryId,
                  double activation,
                  const QString& source,
                  const QString& contextId = QString());
    
    void decay(double elapsedMinutes);
    
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
};

#endif // DESKTOP_PET_ACTIVE_MEMORY_POOL_H
