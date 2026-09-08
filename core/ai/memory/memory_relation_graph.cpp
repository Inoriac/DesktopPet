#include "memory_relation_graph.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <cmath>

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
    // Phase 4.2 新增字段
    relation.provenance = relationProvenanceFromString(
        query.value(QStringLiteral("provenance")).toString());
    relation.supportCount = qMax(1, query.value(QStringLiteral("support_count")).toInt());
    relation.updatedAt = dateTimeFromString(query.value(QStringLiteral("updated_at")).toString());
    relation.lastReinforcedAt = dateTimeFromString(
        query.value(QStringLiteral("last_reinforced_at")).toString());
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
        "(id, from_memory_id, to_memory_id, relation_type, weight, confidence, created_at, payload_json, "
        " provenance, support_count, updated_at, last_reinforced_at) "
        "VALUES (:id, :from, :to, :type, :weight, :confidence, :created_at, :payload_json, "
        " :provenance, :support_count, :updated_at, :last_reinforced_at)"
    ));

    MemoryRelation stored = relation;
    if (stored.id.isEmpty()) {
        stored.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!stored.createdAt.isValid()) {
        stored.createdAt = QDateTime::currentDateTimeUtc();
    }
    if (!stored.updatedAt.isValid()) {
        stored.updatedAt = stored.createdAt;
    }
    stored.supportCount = qMax(1, stored.supportCount);

    query.bindValue(QStringLiteral(":id"), stored.id);
    query.bindValue(QStringLiteral(":from"), stored.fromMemoryId);
    query.bindValue(QStringLiteral(":to"), stored.toMemoryId);
    query.bindValue(QStringLiteral(":type"), memoryRelationTypeToString(stored.type));
    query.bindValue(QStringLiteral(":weight"), stored.weight);
    query.bindValue(QStringLiteral(":confidence"), stored.confidence);
    query.bindValue(QStringLiteral(":created_at"), dateTimeToString(stored.createdAt));
    query.bindValue(QStringLiteral(":payload_json"), jsonObjectToString(stored.payload));
    query.bindValue(QStringLiteral(":provenance"), relationProvenanceToString(stored.provenance));
    query.bindValue(QStringLiteral(":support_count"), stored.supportCount);
    query.bindValue(QStringLiteral(":updated_at"), dateTimeToString(stored.updatedAt));
    query.bindValue(QStringLiteral(":last_reinforced_at"), dateTimeToString(stored.lastReinforcedAt));

    return query.exec();
}

// 合并重复边（设计 §10）：同 (from, to, type) 已存在时强化而非新建：
// support_count+1，weight 取上限叠加（封顶 2.0），刷新 last_reinforced_at。
bool MemoryRelationGraph::addOrReinforceRelation(const MemoryRelation& relation) {
    if (!QSqlDatabase::contains(m_connectionName)) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return false;

    QSqlQuery find(db);
    find.prepare(QStringLiteral(
        "SELECT id, weight, support_count FROM memory_relations "
        "WHERE from_memory_id = :from AND to_memory_id = :to AND relation_type = :type"
    ));
    find.bindValue(QStringLiteral(":from"), relation.fromMemoryId);
    find.bindValue(QStringLiteral(":to"), relation.toMemoryId);
    find.bindValue(QStringLiteral(":type"), memoryRelationTypeToString(relation.type));
    if (!find.exec()) return false;

    if (find.next()) {
        const QString existingId = find.value(0).toString();
        const double existingWeight = find.value(1).toDouble();
        const int existingSupport = qMax(1, find.value(2).toInt());
        const double mergedWeight = qMin(2.0, qMax(existingWeight, relation.weight) + 0.1);

        QSqlQuery update(db);
        update.prepare(QStringLiteral(
            "UPDATE memory_relations SET weight = :weight, support_count = :support, "
            "updated_at = :updated_at, last_reinforced_at = :reinforced "
            "WHERE id = :id"
        ));
        update.bindValue(QStringLiteral(":weight"), mergedWeight);
        update.bindValue(QStringLiteral(":support"), existingSupport + 1);
        update.bindValue(QStringLiteral(":updated_at"),
                         dateTimeToString(QDateTime::currentDateTimeUtc()));
        update.bindValue(QStringLiteral(":reinforced"),
                         dateTimeToString(QDateTime::currentDateTimeUtc()));
        update.bindValue(QStringLiteral(":id"), existingId);
        return update.exec();
    }

    return addRelation(relation);
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

// ===== Phase 4.2 边维护（设计 §10）=====

int MemoryRelationGraph::removeDanglingEdges(const QSet<QString>& validMemoryIds) const {
    if (!QSqlDatabase::contains(m_connectionName)) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return 0;

    int removed = 0;
    const QList<MemoryRelation> relations = all();
    for (const MemoryRelation& relation : relations) {
        const bool dangling = !validMemoryIds.contains(relation.fromMemoryId)
            || !validMemoryIds.contains(relation.toMemoryId);
        if (!dangling) continue;
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM memory_relations WHERE id = :id"));
        del.bindValue(QStringLiteral(":id"), relation.id);
        if (del.exec() && del.numRowsAffected() > 0) {
            ++removed;
        }
    }
    return removed;
}

int MemoryRelationGraph::decayAssociativeEdges(double decayFactor, double removeThreshold) const {
    if (!QSqlDatabase::contains(m_connectionName)) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return 0;

    int removed = 0;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QList<MemoryRelation> relations = all();
    for (const MemoryRelation& relation : relations) {
        // 结构性边不衰减
        if (relation.structural()) continue;

        // 距最后强化/更新的天数 → 累乘衰减
        const QDateTime anchor = relation.lastReinforcedAt.isValid()
            ? relation.lastReinforcedAt : relation.updatedAt;
        const double days = anchor.isValid()
            ? qMax(0.0, static_cast<double>(anchor.daysTo(now))) : 0.0;
        const double newWeight = relation.weight * std::pow(decayFactor, days);

        if (newWeight < removeThreshold) {
            QSqlQuery del(db);
            del.prepare(QStringLiteral("DELETE FROM memory_relations WHERE id = :id"));
            del.bindValue(QStringLiteral(":id"), relation.id);
            if (del.exec() && del.numRowsAffected() > 0) {
                ++removed;
            }
            continue;
        }

        // weight 变化超过浮点噪声才回写
        if (qAbs(newWeight - relation.weight) > 1e-6) {
            QSqlQuery update(db);
            update.prepare(QStringLiteral(
                "UPDATE memory_relations SET weight = :weight, updated_at = :updated_at "
                "WHERE id = :id"));
            update.bindValue(QStringLiteral(":weight"), newWeight);
            update.bindValue(QStringLiteral(":updated_at"), dateTimeToString(now));
            update.bindValue(QStringLiteral(":id"), relation.id);
            update.exec();
        }
    }
    return removed;
}

int MemoryRelationGraph::enforceAssociativeEdgeCap(const QString& memoryId, int cap) const {
    if (!QSqlDatabase::contains(m_connectionName)) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return 0;
    if (cap <= 0) return 0;

    // 收集该节点的非结构性边（两个方向），按 weight 降序
    QList<MemoryRelation> associative;
    const QList<MemoryRelation> neighbors = neighborsOf(memoryId, 10000);
    for (const MemoryRelation& relation : neighbors) {
        if (!relation.structural()) {
            associative.append(relation);
        }
    }
    if (associative.size() <= cap) return 0;

    std::sort(associative.begin(), associative.end(),
              [](const MemoryRelation& a, const MemoryRelation& b) {
                  return a.weight > b.weight;
              });

    int removed = 0;
    for (int i = cap; i < associative.size(); ++i) {
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM memory_relations WHERE id = :id"));
        del.bindValue(QStringLiteral(":id"), associative.at(i).id);
        if (del.exec() && del.numRowsAffected() > 0) {
            ++removed;
        }
    }
    return removed;
}
