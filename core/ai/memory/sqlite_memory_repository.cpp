#include "sqlite_memory_repository.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
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

}

SQLiteMemoryRepository::SQLiteMemoryRepository()
    : m_connectionName(QStringLiteral("memory_") + QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

SQLiteMemoryRepository::~SQLiteMemoryRepository() {
    close();
}

bool SQLiteMemoryRepository::open(const QString& path, QString* errorMessage) {
    if (QSqlDatabase::contains(m_connectionName)) {
        close();
    }

    const QFileInfo info(path);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().path())) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to create directory: %1").arg(info.dir().path());
        return false;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(path);

    if (!db.open()) {
        if (errorMessage) *errorMessage = db.lastError().text();
        QSqlDatabase::removeDatabase(m_connectionName);
        return false;
    }

    {
        QSqlQuery pragma(db);
        pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    }

    if (!initSchema(errorMessage)) {
        db.close();
        QSqlDatabase::removeDatabase(m_connectionName);
        return false;
    }

    return true;
}

void SQLiteMemoryRepository::close() {
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
            if (db.isOpen()) {
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool SQLiteMemoryRepository::isOpen() const {
    if (!QSqlDatabase::contains(m_connectionName)) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    return db.isOpen();
}

bool SQLiteMemoryRepository::initSchema(QString* errorMessage) {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);

    const QStringList statements = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS memory_items ("
            "  id TEXT PRIMARY KEY,"
            "  type TEXT NOT NULL,"
            "  status TEXT NOT NULL,"
            "  privacy_level TEXT NOT NULL,"
            "  key TEXT,"
            "  summary TEXT,"
            "  content TEXT,"
            "  scope TEXT,"
            "  source TEXT,"
            "  importance REAL DEFAULT 0,"
            "  strength REAL DEFAULT 0,"
            "  confidence REAL DEFAULT 0,"
            "  emotion TEXT,"
            "  emotion_intensity REAL DEFAULT 0,"
            "  emotion_confidence REAL DEFAULT 0,"
            "  mention_count INTEGER DEFAULT 0,"
            "  access_count INTEGER DEFAULT 0,"
            "  created_at TEXT,"
            "  updated_at TEXT,"
            "  last_accessed_at TEXT,"
            "  expires_at TEXT,"
            "  payload_json TEXT"
            ")"
        ),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_memory_items_type ON memory_items(type)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_memory_items_status ON memory_items(status)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_memory_items_key ON memory_items(key)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_memory_items_updated_at ON memory_items(updated_at)"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS memory_tags ("
            "  memory_id TEXT NOT NULL,"
            "  tag TEXT NOT NULL,"
            "  PRIMARY KEY(memory_id, tag)"
            ")"
        ),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_memory_tags_tag ON memory_tags(tag)"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS memory_evidence ("
            "  id TEXT PRIMARY KEY,"
            "  memory_id TEXT NOT NULL,"
            "  source TEXT,"
            "  raw_text TEXT,"
            "  created_at TEXT,"
            "  payload_json TEXT"
            ")"
        ),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS memory_relations ("
            "  id TEXT PRIMARY KEY,"
            "  from_memory_id TEXT NOT NULL,"
            "  to_memory_id TEXT NOT NULL,"
            "  relation_type TEXT NOT NULL,"
            "  weight REAL DEFAULT 1.0,"
            "  confidence REAL DEFAULT 1.0,"
            "  created_at TEXT,"
            "  payload_json TEXT"
            ")"
        ),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_memory_relations_from ON memory_relations(from_memory_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_memory_relations_to ON memory_relations(to_memory_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_memory_relations_type ON memory_relations(relation_type)"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS memory_access_log ("
            "  id TEXT PRIMARY KEY,"
            "  memory_id TEXT NOT NULL,"
            "  reason TEXT,"
            "  score REAL DEFAULT 0,"
            "  created_at TEXT"
            ")"
        ),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS memory_embeddings ("
            "  memory_id TEXT NOT NULL,"
            "  model TEXT NOT NULL,"
            "  dimension INTEGER DEFAULT 0,"
            "  vector_blob BLOB,"
            "  content_hash TEXT,"
            "  updated_at TEXT,"
            "  PRIMARY KEY(memory_id, model)"
            ")"
        ),
    };

    for (const QString& sql : statements) {
        if (!query.exec(sql)) {
            if (errorMessage) *errorMessage = query.lastError().text();
            return false;
        }
    }

    return true;
}

bool SQLiteMemoryRepository::insert(const MemoryEntry& entry) {
    if (!isOpen()) return false;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);

    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO memory_items ("
        "  id, type, status, privacy_level, key, summary, content,"
        "  scope, source, importance, strength, confidence,"
        "  emotion, emotion_intensity, emotion_confidence,"
        "  mention_count, access_count,"
        "  created_at, updated_at, last_accessed_at, expires_at,"
        "  payload_json"
        ") VALUES ("
        "  :id, :type, :status, :privacy_level, :key, :summary, :content,"
        "  :scope, :source, :importance, :strength, :confidence,"
        "  :emotion, :emotion_intensity, :emotion_confidence,"
        "  :mention_count, :access_count,"
        "  :created_at, :updated_at, :last_accessed_at, :expires_at,"
        "  :payload_json"
        ")"
    ));

    QJsonObject payload = entry.payload;
    if (!entry.value.isNull() && !entry.value.isUndefined()) {
        payload[QStringLiteral("value")] = entry.value;
    }

    query.bindValue(QStringLiteral(":id"), entry.id);
    query.bindValue(QStringLiteral(":type"), memoryTypeToString(entry.type));
    query.bindValue(QStringLiteral(":status"), memoryStatusToString(entry.status));
    query.bindValue(QStringLiteral(":privacy_level"), privacyLevelToString(entry.privacyLevel));
    query.bindValue(QStringLiteral(":key"), entry.key);
    query.bindValue(QStringLiteral(":summary"), entry.summary);
    query.bindValue(QStringLiteral(":content"), entry.content);
    query.bindValue(QStringLiteral(":scope"), entry.scope);
    query.bindValue(QStringLiteral(":source"), entry.source);
    query.bindValue(QStringLiteral(":importance"), entry.importance);
    query.bindValue(QStringLiteral(":strength"), entry.strength);
    query.bindValue(QStringLiteral(":confidence"), entry.confidence);
    query.bindValue(QStringLiteral(":emotion"), emotionTypeToString(entry.emotion));
    query.bindValue(QStringLiteral(":emotion_intensity"), entry.emotionIntensity);
    query.bindValue(QStringLiteral(":emotion_confidence"), entry.emotionConfidence);
    query.bindValue(QStringLiteral(":mention_count"), entry.mentionCount);
    query.bindValue(QStringLiteral(":access_count"), entry.accessCount);
    query.bindValue(QStringLiteral(":created_at"), dateTimeToString(entry.createdAt));
    query.bindValue(QStringLiteral(":updated_at"), dateTimeToString(entry.updatedAt));
    query.bindValue(QStringLiteral(":last_accessed_at"), dateTimeToString(entry.lastAccessedAt));
    query.bindValue(QStringLiteral(":expires_at"), dateTimeToString(entry.expiresAt));
    query.bindValue(QStringLiteral(":payload_json"), jsonObjectToString(payload));

    if (!query.exec()) {
        return false;
    }

    deleteTags(entry.id);
    insertTags(entry.id, entry.tags);

    deleteEvidence(entry.id);
    insertEvidence(entry.id, entry.evidence);

    return true;
}

bool SQLiteMemoryRepository::update(const MemoryEntry& entry) {
    return insert(entry);
}

bool SQLiteMemoryRepository::updateStatus(const QString& id,
                                           MemoryStatus status,
                                           const QJsonObject& payloadPatch) {
    if (!isOpen()) return false;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    const QDateTime now = QDateTime::currentDateTimeUtc();

    if (payloadPatch.isEmpty()) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
            "UPDATE memory_items SET status = :status, updated_at = :updated_at WHERE id = :id"
        ));
        query.bindValue(QStringLiteral(":status"), memoryStatusToString(status));
        query.bindValue(QStringLiteral(":updated_at"), dateTimeToString(now));
        query.bindValue(QStringLiteral(":id"), id);
        return query.exec() && query.numRowsAffected() > 0;
    }

    QSqlQuery selectQuery(db);
    selectQuery.prepare(QStringLiteral("SELECT payload_json FROM memory_items WHERE id = :id"));
    selectQuery.bindValue(QStringLiteral(":id"), id);
    if (!selectQuery.exec() || !selectQuery.next()) return false;

    QJsonObject payload = jsonObjectFromString(selectQuery.value(0).toString());
    for (auto it = payloadPatch.constBegin(); it != payloadPatch.constEnd(); ++it) {
        payload[it.key()] = it.value();
    }

    QSqlQuery updateQuery(db);
    updateQuery.prepare(QStringLiteral(
        "UPDATE memory_items SET status = :status, updated_at = :updated_at, payload_json = :payload_json WHERE id = :id"
    ));
    updateQuery.bindValue(QStringLiteral(":status"), memoryStatusToString(status));
    updateQuery.bindValue(QStringLiteral(":updated_at"), dateTimeToString(now));
    updateQuery.bindValue(QStringLiteral(":payload_json"), jsonObjectToString(payload));
    updateQuery.bindValue(QStringLiteral(":id"), id);
    return updateQuery.exec() && updateQuery.numRowsAffected() > 0;
}

QList<MemoryEntry> SQLiteMemoryRepository::loadAll() {
    QList<MemoryEntry> entries;
    if (!isOpen()) return entries;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);

    if (!query.exec(QStringLiteral("SELECT * FROM memory_items ORDER BY created_at ASC"))) {
        return entries;
    }

    while (query.next()) {
        MemoryEntry entry;
        entry.id = query.value(QStringLiteral("id")).toString();
        entry.type = memoryTypeFromString(query.value(QStringLiteral("type")).toString());
        entry.status = memoryStatusFromString(query.value(QStringLiteral("status")).toString());
        entry.privacyLevel = privacyLevelFromString(query.value(QStringLiteral("privacy_level")).toString());
        entry.key = query.value(QStringLiteral("key")).toString();
        entry.summary = query.value(QStringLiteral("summary")).toString();
        entry.content = query.value(QStringLiteral("content")).toString();
        entry.scope = query.value(QStringLiteral("scope")).toString();
        entry.source = query.value(QStringLiteral("source")).toString();
        entry.importance = query.value(QStringLiteral("importance")).toDouble();
        entry.strength = query.value(QStringLiteral("strength")).toDouble();
        entry.confidence = query.value(QStringLiteral("confidence")).toDouble();
        entry.emotion = emotionTypeFromString(query.value(QStringLiteral("emotion")).toString());
        entry.emotionIntensity = query.value(QStringLiteral("emotion_intensity")).toDouble();
        entry.emotionConfidence = query.value(QStringLiteral("emotion_confidence")).toDouble();
        entry.mentionCount = query.value(QStringLiteral("mention_count")).toInt();
        entry.accessCount = query.value(QStringLiteral("access_count")).toInt();
        entry.createdAt = dateTimeFromString(query.value(QStringLiteral("created_at")).toString());
        entry.updatedAt = dateTimeFromString(query.value(QStringLiteral("updated_at")).toString());
        entry.lastAccessedAt = dateTimeFromString(query.value(QStringLiteral("last_accessed_at")).toString());
        entry.expiresAt = dateTimeFromString(query.value(QStringLiteral("expires_at")).toString());

        QJsonObject payload = jsonObjectFromString(query.value(QStringLiteral("payload_json")).toString());
        if (payload.contains(QStringLiteral("value"))) {
            entry.value = payload.take(QStringLiteral("value"));
        }
        entry.payload = payload;

        entry.tags = loadTags(entry.id);
        entry.evidence = loadEvidence(entry.id);

        if (entry.summary.trimmed().isEmpty() && !entry.content.trimmed().isEmpty()) {
            entry.summary = entry.content.trimmed();
        }
        if (entry.createdAt.isValid() && !entry.updatedAt.isValid()) {
            entry.updatedAt = entry.createdAt;
        }

        entries.append(entry);
    }

    return entries;
}

bool SQLiteMemoryRepository::clear() {
    if (!isOpen()) return false;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);

    query.exec(QStringLiteral("DELETE FROM memory_tags"));
    query.exec(QStringLiteral("DELETE FROM memory_evidence"));
    query.exec(QStringLiteral("DELETE FROM memory_relations"));
    query.exec(QStringLiteral("DELETE FROM memory_access_log"));
    query.exec(QStringLiteral("DELETE FROM memory_embeddings"));
    query.exec(QStringLiteral("DELETE FROM memory_items"));
    return true;
}

bool SQLiteMemoryRepository::insertTags(const QString& memoryId, const QStringList& tags) {
    if (tags.isEmpty()) return true;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO memory_tags (memory_id, tag) VALUES (:memory_id, :tag)"
    ));

    for (const QString& tag : tags) {
        if (tag.trimmed().isEmpty()) continue;
        query.bindValue(QStringLiteral(":memory_id"), memoryId);
        query.bindValue(QStringLiteral(":tag"), tag);
        query.exec();
    }
    return true;
}

bool SQLiteMemoryRepository::insertEvidence(const QString& memoryId, const QStringList& evidence) {
    if (evidence.isEmpty()) return true;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO memory_evidence (id, memory_id, raw_text, created_at)"
        " VALUES (:id, :memory_id, :raw_text, :created_at)"
    ));

    const QString now = dateTimeToString(QDateTime::currentDateTimeUtc());
    for (const QString& text : evidence) {
        if (text.trimmed().isEmpty()) continue;
        query.bindValue(QStringLiteral(":id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        query.bindValue(QStringLiteral(":memory_id"), memoryId);
        query.bindValue(QStringLiteral(":raw_text"), text);
        query.bindValue(QStringLiteral(":created_at"), now);
        query.exec();
    }
    return true;
}

void SQLiteMemoryRepository::deleteTags(const QString& memoryId) {
    if (!isOpen()) return;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM memory_tags WHERE memory_id = :memory_id"));
    query.bindValue(QStringLiteral(":memory_id"), memoryId);
    query.exec();
}

void SQLiteMemoryRepository::deleteEvidence(const QString& memoryId) {
    if (!isOpen()) return;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM memory_evidence WHERE memory_id = :memory_id"));
    query.bindValue(QStringLiteral(":memory_id"), memoryId);
    query.exec();
}

QStringList SQLiteMemoryRepository::loadTags(const QString& memoryId) {
    QStringList tags;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT tag FROM memory_tags WHERE memory_id = :memory_id"));
    query.bindValue(QStringLiteral(":memory_id"), memoryId);
    if (query.exec()) {
        while (query.next()) {
            tags.append(query.value(0).toString());
        }
    }
    return tags;
}

QStringList SQLiteMemoryRepository::loadEvidence(const QString& memoryId) {
    QStringList evidence;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT raw_text FROM memory_evidence WHERE memory_id = :memory_id ORDER BY created_at ASC"));
    query.bindValue(QStringLiteral(":memory_id"), memoryId);
    if (query.exec()) {
        while (query.next()) {
            evidence.append(query.value(0).toString());
        }
    }
    return evidence;
}
