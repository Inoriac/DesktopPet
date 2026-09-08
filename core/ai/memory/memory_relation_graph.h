#ifndef DESKTOP_PET_MEMORY_RELATION_GRAPH_H
#define DESKTOP_PET_MEMORY_RELATION_GRAPH_H

#include <QList>
#include <QSet>
#include <QString>

#include "memory_relation.h"

class QSqlDatabase;

class MemoryRelationGraph {
public:
    void setConnectionName(const QString& connectionName);

    bool addRelation(const MemoryRelation& relation);
    // 合并重复边：同 (from,to,type) 已存在则强化（support_count+1，weight 封顶 2.0）
    bool addOrReinforceRelation(const MemoryRelation& relation);
    bool removeRelation(const QString& relationId);
    bool removeRelationsFor(const QString& memoryId);

    QList<MemoryRelation> neighborsOf(const QString& memoryId, int limit = 20) const;
    QList<MemoryRelation> neighborsOf(const QString& memoryId,
                                      MemoryRelationType type,
                                      int limit = 20) const;
    bool hasRelation(const QString& fromId, const QString& toId,
                     MemoryRelationType type) const;

    QList<MemoryRelation> all() const;

    // ===== Phase 4.2 边维护（设计 §10）=====
    // 悬空边清理：删除两端任一记忆已不存在的边。返回删除数。
    int removeDanglingEdges(const QSet<QString>& validMemoryIds) const;
    // 弱联想边衰减：仅 Related/MentionedWith/非确认 TopicOf，
    // weight *= decayFactor（距 last_reinforced_at/updated_at 越久衰减越多）；
    // weight 低于 threshold 的边删除。结构性边不受影响。返回删除数。
    int decayAssociativeEdges(double decayFactor, double removeThreshold) const;
    // 节点联想边上限（默认 32）：超出时删除 weight 最低的联想边。
    // 结构性边不计入上限。返回删除数。
    int enforceAssociativeEdgeCap(const QString& memoryId, int cap = 32) const;

private:
    MemoryRelation relationFromQuery(const QSqlDatabase& db, int row) const;

    QString m_connectionName;
};

#endif // DESKTOP_PET_MEMORY_RELATION_GRAPH_H
