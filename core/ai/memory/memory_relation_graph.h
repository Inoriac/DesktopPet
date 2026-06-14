#ifndef DESKTOP_PET_MEMORY_RELATION_GRAPH_H
#define DESKTOP_PET_MEMORY_RELATION_GRAPH_H

#include <QList>
#include <QString>

#include "memory_relation.h"

class QSqlDatabase;

class MemoryRelationGraph {
public:
    void setConnectionName(const QString& connectionName);

    bool addRelation(const MemoryRelation& relation);
    bool removeRelation(const QString& relationId);
    bool removeRelationsFor(const QString& memoryId);

    QList<MemoryRelation> neighborsOf(const QString& memoryId, int limit = 20) const;
    QList<MemoryRelation> neighborsOf(const QString& memoryId,
                                      MemoryRelationType type,
                                      int limit = 20) const;
    bool hasRelation(const QString& fromId, const QString& toId,
                     MemoryRelationType type) const;

    QList<MemoryRelation> all() const;

private:
    MemoryRelation relationFromQuery(const QSqlDatabase& db, int row) const;

    QString m_connectionName;
};

#endif // DESKTOP_PET_MEMORY_RELATION_GRAPH_H
