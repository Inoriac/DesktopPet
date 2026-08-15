#include "tag_cooccurrence_graph.h"

#include <algorithm>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

namespace {

constexpr int MAX_TAGS_PER_MEMORY = 16;

QStringList normalizedTags(const QStringList& tags) {
    QStringList result;
    for (const QString& rawTag : tags) {
        const QString tag = rawTag.simplified().toCaseFolded().left(64);
        if (!tag.isEmpty() && !result.contains(tag)) {
            result.append(tag);
        }
    }
    std::sort(result.begin(), result.end(), [](const QString& a, const QString& b) {
        return QString::compare(a, b, Qt::CaseSensitive) < 0;
    });
    return result.mid(0, MAX_TAGS_PER_MEMORY);
}

QDateTime dateTimeFromString(const QString& value) {
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed.isValid() ? parsed : QDateTime{};
}

TagCooccurrence readEdge(QSqlQuery& query) {
    TagCooccurrence edge;
    edge.tagA = query.value(QStringLiteral("tag_a")).toString();
    edge.tagB = query.value(QStringLiteral("tag_b")).toString();
    edge.weight = query.value(QStringLiteral("weight")).toInt();
    edge.updatedAt = dateTimeFromString(query.value(QStringLiteral("updated_at")).toString());
    return edge;
}

} // namespace

void TagCooccurrenceGraph::setConnectionName(const QString& connectionName) {
    m_connectionName = connectionName;
}

bool TagCooccurrenceGraph::recordTags(const QStringList& tags) {
    if (!QSqlDatabase::contains(m_connectionName)) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return false;

    const QStringList normalized = normalizedTags(tags);
    if (normalized.size() < 2) return true;

    const QString savepoint = QStringLiteral("tag_pairs_%1")
                                  .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-'));
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SAVEPOINT %1").arg(savepoint))) return false;
    const auto rollback = [&]() {
        query.exec(QStringLiteral("ROLLBACK TO SAVEPOINT %1").arg(savepoint));
        query.exec(QStringLiteral("RELEASE SAVEPOINT %1").arg(savepoint));
    };

    query.prepare(QStringLiteral(
        "INSERT INTO tag_cooccurrences (tag_a, tag_b, weight, updated_at) "
        "VALUES (:tag_a, :tag_b, 1, :updated_at) "
        "ON CONFLICT(tag_a, tag_b) DO UPDATE SET "
        "weight = tag_cooccurrences.weight + 1, updated_at = excluded.updated_at"
    ));
    const QString updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    for (int i = 0; i < normalized.size(); ++i) {
        for (int j = i + 1; j < normalized.size(); ++j) {
            query.bindValue(QStringLiteral(":tag_a"), normalized.at(i));
            query.bindValue(QStringLiteral(":tag_b"), normalized.at(j));
            query.bindValue(QStringLiteral(":updated_at"), updatedAt);
            if (!query.exec()) {
                rollback();
                return false;
            }
        }
    }
    return query.exec(QStringLiteral("RELEASE SAVEPOINT %1").arg(savepoint));
}

int TagCooccurrenceGraph::weightBetween(const QString& firstTag,
                                        const QString& secondTag) const {
    const QStringList normalized = normalizedTags({firstTag, secondTag});
    if (normalized.size() != 2 || !QSqlDatabase::contains(m_connectionName)) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return 0;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT weight FROM tag_cooccurrences WHERE tag_a = :tag_a AND tag_b = :tag_b"
    ));
    query.bindValue(QStringLiteral(":tag_a"), normalized.at(0));
    query.bindValue(QStringLiteral(":tag_b"), normalized.at(1));
    return query.exec() && query.next() ? query.value(0).toInt() : 0;
}

QList<TagCooccurrence> TagCooccurrenceGraph::neighborsOf(const QString& tag,
                                                         int limit) const {
    QList<TagCooccurrence> result;
    const QStringList normalized = normalizedTags({tag});
    if (normalized.isEmpty() || limit <= 0 || !QSqlDatabase::contains(m_connectionName)) {
        return result;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return result;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT tag_a, tag_b, weight, updated_at FROM tag_cooccurrences "
        "WHERE tag_a = :tag_a OR tag_b = :tag_b "
        "ORDER BY weight DESC, tag_a ASC, tag_b ASC LIMIT :limit"
    ));
    query.bindValue(QStringLiteral(":tag_a"), normalized.first());
    query.bindValue(QStringLiteral(":tag_b"), normalized.first());
    query.bindValue(QStringLiteral(":limit"), limit);
    if (query.exec()) {
        while (query.next()) result.append(readEdge(query));
    }
    return result;
}
