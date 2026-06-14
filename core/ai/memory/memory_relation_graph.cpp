#include "memory_relation_graph.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

namespace {

QString dateTimeToString(const QDateTime& value) {
    return value.isValid() ? value.toString(Qt::ISODate) : QString();
}

QDateTime dateTimeFromString(const QString& value) {
    if (value.isEmpty()) return {};
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed.isValid() ? parsed : QDateTime{};
}

QString jsonObjectToString(const QJsonObject& obj) {
    if (obj.isEmpty()) return {};
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QJsonObject jsonObjectFromString(const QString& text) {
    if (text.isEmpty()) return {};
    return QJsonDocument::fromJson(text.toUtf8()).object();
}

MemoryRelation readRelation(QSqlQuery& query) {
    MemoryRelation relation;
    relation.id = query.value(QStringLiteral("id")).toString();
    relation.fromMemoryId = query.value(QStringLiteral("from_memory_id")).toString();
    relation.toMemoryId = query.value(QStringLiteral("to_memory_id")).toString();
    relation.type = memoryRelationTypeFromString(query.value(QStringLiteral("relation_type")).toString());
    relation.weight = query.value(QStringLiteral("weight")).toDouble();
    relation.confidence = query.value(QStringLiteral("confidence")).toDouble();
    relation.createdAt = dateTimeFromString(query.value(QStringLiteral("created_at")).toString());
    relation.payload = jsonObjectFromString(query.value(QStringLiteral("payload_json")).toString());
    return relation;
}

}

void MemoryRelationGraph::setConnectionName(const QString& connectionName) {
    m_connectionName = connectionName;
}

bool MemoryRelationGraph::addRelation(const MemoryRelation& relation) {
    if (!QSqlDatabase::contains(m_connectionName)) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO memory_relations "
        "(id, from_memory_id, to_memory_id, relation_type, weight, confidence, created_at, payload_json) "
        "VALUES (:id, :from, :to, :type, :weight, :confidence, :created_at, :payload_json)"
    ));

    MemoryRelation stored = relation;
    if (stored.id.isEmpty()) {
        stored.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!stored.createdAt.isValid()) {
        stored.createdAt = QDateTime::currentDateTimeUtc();
    }

    query.bindValue(QStringLiteral(":id"), stored.id);
    query.bindValue(QStringLiteral(":from"), stored.fromMemoryId);
    query.bindValue(QStringLiteral(":to"), stored.toMemoryId);
    query.bindValue(QStringLiteral(":type"), memoryRelationTypeToString(stored.type));
    query.bindValue(QStringLiteral(":weight"), stored.weight);
    query.bindValue(QStringLiteral(":confidence"), stored.confidence);
    query.bindValue(QStringLiteral(":created_at"), dateTimeToString(stored.createdAt));
    query.bindValue(QStringLiteral(":payload_json"), jsonObjectToString(stored.payload));

    return query.exec();
}

bool MemoryRelationGraph::removeRelation(const QString& relationId) {
    if (!QSqlDatabase::contains(m_connectionName)) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM memory_relations WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), relationId);
    return query.exec() && query.numRowsAffected() > 0;
}

bool MemoryRelationGraph::removeRelationsFor(const QString& memoryId) {
    if (!QSqlDatabase::contains(m_connectionName)) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "DELETE FROM memory_relations WHERE from_memory_id = :id OR to_memory_id = :id"
    ));
    query.bindValue(QStringLiteral(":id"), memoryId);
    return query.exec();
}

QList<MemoryRelation> MemoryRelationGraph::neighborsOf(const QString& memoryId, int limit) const {
    QList<MemoryRelation> results;
    if (!QSqlDatabase::contains(m_connectionName)) return results;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return results;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT * FROM memory_relations "
        "WHERE from_memory_id = :id1 OR to_memory_id = :id2 "
        "ORDER BY weight DESC LIMIT :limit"
    ));
    query.bindValue(QStringLiteral(":id1"), memoryId);
    query.bindValue(QStringLiteral(":id2"), memoryId);
    query.bindValue(QStringLiteral(":limit"), limit);

    if (query.exec()) {
        while (query.next()) {
            results.append(readRelation(query));
        }
    }
    return results;
}

QList<MemoryRelation> MemoryRelationGraph::neighborsOf(const QString& memoryId,
                                                        MemoryRelationType type,
                                                        int limit) const {
    QList<MemoryRelation> results;
    if (!QSqlDatabase::contains(m_connectionName)) return results;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return results;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT * FROM memory_relations "
        "WHERE (from_memory_id = :id1 OR to_memory_id = :id2) AND relation_type = :type "
        "ORDER BY weight DESC LIMIT :limit"
    ));
    query.bindValue(QStringLiteral(":id1"), memoryId);
    query.bindValue(QStringLiteral(":id2"), memoryId);
    query.bindValue(QStringLiteral(":type"), memoryRelationTypeToString(type));
    query.bindValue(QStringLiteral(":limit"), limit);

    if (query.exec()) {
        while (query.next()) {
            results.append(readRelation(query));
        }
    }
    return results;
}

bool MemoryRelationGraph::hasRelation(const QString& fromId, const QString& toId,
                                       MemoryRelationType type) const {
    if (!QSqlDatabase::contains(m_connectionName)) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM memory_relations "
        "WHERE from_memory_id = :from AND to_memory_id = :to AND relation_type = :type"
    ));
    query.bindValue(QStringLiteral(":from"), fromId);
    query.bindValue(QStringLiteral(":to"), toId);
    query.bindValue(QStringLiteral(":type"), memoryRelationTypeToString(type));

    if (query.exec() && query.next()) {
        return query.value(0).toInt() > 0;
    }
    return false;
}

QList<MemoryRelation> MemoryRelationGraph::all() const {
    QList<MemoryRelation> results;
    if (!QSqlDatabase::contains(m_connectionName)) return results;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return results;

    QSqlQuery query(db);
    if (query.exec(QStringLiteral("SELECT * FROM memory_relations ORDER BY created_at ASC"))) {
        while (query.next()) {
            results.append(readRelation(query));
        }
    }
    return results;
}
