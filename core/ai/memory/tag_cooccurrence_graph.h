#ifndef DESKTOP_PET_TAG_COOCCURRENCE_GRAPH_H
#define DESKTOP_PET_TAG_COOCCURRENCE_GRAPH_H

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

struct TagCooccurrence {
    QString tagA;
    QString tagB;
    int weight = 0;
    QDateTime updatedAt;
};

// SQLite-backed aggregate graph. Writes reuse MemoryStore's connection so an
// outer Daydream transaction commits or rolls back memories and tag edges together.
class TagCooccurrenceGraph {
public:
    void setConnectionName(const QString& connectionName);

    // Each call records one accepted consolidation event for every tag pair.
    bool recordTags(const QStringList& tags);
    int weightBetween(const QString& firstTag, const QString& secondTag) const;
    QList<TagCooccurrence> neighborsOf(const QString& tag, int limit = 20) const;

private:
    QString m_connectionName;
};

#endif // DESKTOP_PET_TAG_COOCCURRENCE_GRAPH_H
