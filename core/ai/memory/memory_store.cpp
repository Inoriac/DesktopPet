#include "memory_store.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

#include <utility>

#include "memory_repository.h"
#include "partition_policy.h"
#include "sqlite_memory_repository.h"

namespace {

QJsonArray stringListToJson(const QStringList& values) {
    QJsonArray array;
    for (const QString& value : values) {
        array.append(value);
    }
    return array;
}

QStringList stringListFromJson(const QJsonArray& array) {
    QStringList values;
    for (const QJsonValue& value : array) {
        values.append(value.toString());
    }
    return values;
}

QString dateTimeToString(const QDateTime& value) {
    return value.isValid() ? value.toString(Qt::ISODateWithMs) : QString();
}

QDateTime dateTimeFromString(const QString& value) {
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(value, Qt::ISODate);
    }
    return parsed.isValid() ? parsed : QDateTime{};
}

QString valueToCompactText(const QJsonValue& value) {
    if (value.isString()) return value.toString();
    if (value.isUndefined() || value.isNull()) return {};
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"value", value}}).toJson(QJsonDocument::Compact));
}

QString fallbackSummary(const MemoryEntry& entry) {
    if (!entry.summary.trimmed().isEmpty()) return entry.summary.trimmed();
    if (!entry.content.trimmed().isEmpty()) return entry.content.trimmed();
    const QString valueText = valueToCompactText(entry.value);
    if (!entry.key.trimmed().isEmpty() && !valueText.trimmed().isEmpty()) {
        return QString("%1=%2").arg(entry.key, valueText);
    }
    if (!valueText.trimmed().isEmpty()) return valueText;
    return entry.key;
}

QString confidenceLabel(double confidence) {
    if (confidence >= 0.85) return QStringLiteral("高置信度");
    if (confidence >= 0.55) return QStringLiteral("中置信度");
    if (confidence > 0.0) return QStringLiteral("低置信度");
    return {};
}

bool shouldInjectIntoContext(const MemoryEntry& entry) {
    return entry.status == MemoryStatus::Active
        && entry.privacyLevel != PrivacyLevel::Sensitive;
}

}

QJsonObject MemoryEntry::toJson() const {
    QJsonObject obj;
    obj["id"] = id;
    obj["type"] = memoryTypeToString(type);
    obj["status"] = memoryStatusToString(status);
    obj["privacy_level"] = privacyLevelToString(privacyLevel);
    obj["partition"] = partition;
    obj["key"] = key;
    obj["value"] = value;
    obj["summary"] = summary;
    obj["content"] = content;
    obj["tags"] = stringListToJson(tags);
    obj["scope"] = scope;
    obj["source"] = source;
    obj["importance"] = importance;
    obj["strength"] = strength;
    obj["confidence"] = confidence;
    obj["emotion"] = emotionTypeToString(emotion);
    obj["emotion_intensity"] = emotionIntensity;
    obj["emotion_confidence"] = emotionConfidence;
    obj["mention_count"] = mentionCount;
    obj["access_count"] = accessCount;
    obj["created_at"] = dateTimeToString(createdAt);
    obj["updated_at"] = dateTimeToString(updatedAt);
    obj["last_accessed_at"] = dateTimeToString(lastAccessedAt);
    obj["expires_at"] = dateTimeToString(expiresAt);
    obj["evidence"] = stringListToJson(evidence);
    obj["source_memory_ids"] = stringListToJson(sourceMemoryIds);
    obj["supersedes"] = stringListToJson(supersedes);
    obj["conflicts_with"] = stringListToJson(conflictsWith);
    obj["payload"] = payload;
    return obj;
}

MemoryEntry MemoryEntry::fromJson(const QJsonObject& object) {
    MemoryEntry entry;
    entry.id = object.value("id").toString();
    entry.type = memoryTypeFromString(object.value("type").toString());
    entry.status = memoryStatusFromString(object.value("status").toString("active"));
    entry.privacyLevel = privacyLevelFromString(object.value("privacy_level").toString("public"));
    entry.partition = object.value("partition").toString();
    if (entry.partition.trimmed().isEmpty()) {
        entry.partition = partitionToString(partitionForType(entry.type));  // 旧 JSON 兼容：派生自 type
    }
    entry.key = object.value("key").toString();
    entry.value = object.value("value");
    entry.summary = object.value("summary").toString();
    entry.content = object.value("content").toString();
    entry.tags = stringListFromJson(object.value("tags").toArray());
    entry.scope = object.value("scope").toString();
    entry.source = object.value("source").toString();
    entry.importance = object.value("importance").toDouble(0.0);
    entry.strength = object.value("strength").toDouble(0.0);
    entry.confidence = object.value("confidence").toDouble(0.0);
    entry.emotion = emotionTypeFromString(object.value("emotion").toString("neutral"));
    entry.emotionIntensity = object.value("emotion_intensity").toDouble(0.0);
    entry.emotionConfidence = object.value("emotion_confidence").toDouble(0.0);
    entry.mentionCount = object.value("mention_count").toInt(0);
    entry.accessCount = object.value("access_count").toInt(0);
    entry.createdAt = dateTimeFromString(object.value("created_at").toString());
    entry.updatedAt = dateTimeFromString(object.value("updated_at").toString());
    entry.lastAccessedAt = dateTimeFromString(object.value("last_accessed_at").toString());
    entry.expiresAt = dateTimeFromString(object.value("expires_at").toString());
    entry.evidence = stringListFromJson(object.value("evidence").toArray());
    entry.sourceMemoryIds = stringListFromJson(object.value("source_memory_ids").toArray());
    entry.supersedes = stringListFromJson(object.value("supersedes").toArray());
    entry.conflictsWith = stringListFromJson(object.value("conflicts_with").toArray());
    entry.payload = object.value("payload").toObject();

    if (entry.summary.trimmed().isEmpty()) {
        entry.summary = fallbackSummary(entry);
    }
    if (entry.createdAt.isValid() && !entry.updatedAt.isValid()) {
        entry.updatedAt = entry.createdAt;
    }
    return entry;
}

MemoryStore::MemoryStore()
    : m_repository(std::make_unique<SQLiteMemoryRepository>()) {}

MemoryStore::~MemoryStore() = default;

void MemoryStore::setStoragePath(const QString& memoryFilePath) {
    m_memoryFilePath = memoryFilePath;
}

void MemoryStore::setDatabasePath(const QString& dbPath) {
    m_databasePath = dbPath;
}

QString MemoryStore::databaseConnectionName() const {
    return m_repository ? m_repository->connectionName() : QString();
}

bool MemoryStore::beginTransaction() {
    return m_repository && m_repository->beginTransaction();
}

bool MemoryStore::commitTransaction() {
    return m_repository && m_repository->commitTransaction();
}

bool MemoryStore::rollbackTransaction() {
    return m_repository && m_repository->rollbackTransaction();
}

bool MemoryStore::stageSleepChange(const StagedMemoryChange& change) {
    if (!m_repository || !m_repository->isOpen()
        || change.sessionId.trimmed().isEmpty()
        || change.changeId.trimmed().isEmpty()
        || change.targetType.trimmed().isEmpty()
        || change.operation.trimmed().isEmpty()) {
        return false;
    }
    const QByteArray payload = QJsonDocument(change.payload)
                                   .toJson(QJsonDocument::Compact);
    const QString payloadHash = QString::fromLatin1(
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
    QSqlDatabase database = QSqlDatabase::database(m_repository->connectionName());
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO sleep_staged_change("
        "session_id,change_id,target_type,operation,target_id,payload_json,"
        "payload_hash,status,created_at) VALUES(?,?,?,?,?,?,?,'Prepared',?)"));
    insert.addBindValue(change.sessionId);
    insert.addBindValue(change.changeId);
    insert.addBindValue(change.targetType);
    insert.addBindValue(change.operation);
    insert.addBindValue(change.targetId);
    insert.addBindValue(QString::fromUtf8(payload));
    insert.addBindValue(payloadHash);
    insert.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!insert.exec()) return false;

    QSqlQuery verify(database);
    verify.prepare(QStringLiteral(
        "SELECT target_type,operation,target_id,payload_hash FROM sleep_staged_change "
        "WHERE session_id=? AND change_id=?"));
    verify.addBindValue(change.sessionId);
    verify.addBindValue(change.changeId);
    return verify.exec() && verify.next()
        && verify.value(0).toString() == change.targetType
        && verify.value(1).toString() == change.operation
        && verify.value(2).toString() == change.targetId
        && verify.value(3).toString() == payloadHash;
}

Result<QList<StagedMemoryChange>, DomainError> MemoryStore::preparedSleepChanges(
    const QString& sessionId,
    const QString& targetType) const {
    QList<StagedMemoryChange> changes;
    if (!m_repository || !m_repository->isOpen()
        || sessionId.trimmed().isEmpty()) {
        return Result<QList<StagedMemoryChange>, DomainError>::failure(
            domainError(QStringLiteral("MEMORY_STORE_UNAVAILABLE"),
                        QStringLiteral("memory sleep staging is unavailable")));
    }
    QSqlQuery query(QSqlDatabase::database(m_repository->connectionName()));
    QString sql = QStringLiteral(
        "SELECT session_id,change_id,target_type,operation,target_id,payload_json,"
        "payload_hash FROM sleep_staged_change "
        "WHERE session_id=? AND status IN ('Prepared','Finalized')");
    if (!targetType.trimmed().isEmpty()) sql += QStringLiteral(" AND target_type=?");
    sql += QStringLiteral(" ORDER BY created_at,change_id");
    query.prepare(sql);
    query.addBindValue(sessionId);
    if (!targetType.trimmed().isEmpty()) query.addBindValue(targetType);
    if (!query.exec()) {
        return Result<QList<StagedMemoryChange>, DomainError>::failure(
            domainError(QStringLiteral("MEMORY_STORE_UNAVAILABLE"),
                        QStringLiteral("failed to read memory sleep staging")));
    }
    while (query.next()) {
        QJsonParseError parseError;
        const QByteArray payloadBytes = query.value(5).toByteArray();
        const QString expectedHash = QString::fromLatin1(
            QCryptographicHash::hash(payloadBytes, QCryptographicHash::Sha256).toHex());
        const QJsonDocument document = QJsonDocument::fromJson(payloadBytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            return Result<QList<StagedMemoryChange>, DomainError>::failure(
                domainError(QStringLiteral("MEMORY_STORE_UNAVAILABLE"),
                            QStringLiteral("memory sleep staging payload is invalid")));
        }
        if (query.value(6).toString() != expectedHash) {
            return Result<QList<StagedMemoryChange>, DomainError>::failure(
                domainError(QStringLiteral("MEMORY_STORE_UNAVAILABLE"),
                            QStringLiteral("memory sleep staging hash mismatch")));
        }
        StagedMemoryChange change;
        change.sessionId = query.value(0).toString();
        change.changeId = query.value(1).toString();
        change.targetType = query.value(2).toString();
        change.operation = query.value(3).toString();
        change.targetId = query.value(4).toString();
        change.payload = document.object();
        change.payloadHash = query.value(6).toString();
        changes.append(std::move(change));
    }
    return Result<QList<StagedMemoryChange>, DomainError>::success(
        std::move(changes));
}

bool MemoryStore::markSleepChangeFinalized(const QString& sessionId,
                                           const QString& changeId) {
    if (!m_repository || !m_repository->isOpen()) return false;
    QSqlQuery query(QSqlDatabase::database(m_repository->connectionName()));
    query.prepare(QStringLiteral(
        "UPDATE sleep_staged_change SET status='Finalized',finalized_at=? "
        "WHERE session_id=? AND change_id=? AND status IN ('Prepared','Finalized')"));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(sessionId);
    query.addBindValue(changeId);
    if (!query.exec()) return false;
    QSqlQuery verify(QSqlDatabase::database(m_repository->connectionName()));
    verify.prepare(QStringLiteral(
        "SELECT status FROM sleep_staged_change WHERE session_id=? AND change_id=?"));
    verify.addBindValue(sessionId);
    verify.addBindValue(changeId);
    return verify.exec() && verify.next()
        && verify.value(0).toString() == QLatin1String("Finalized");
}

bool MemoryStore::abortSleepChanges(const QString& sessionId) {
    if (!m_repository || !m_repository->isOpen()) return false;
    QSqlQuery query(QSqlDatabase::database(m_repository->connectionName()));
    query.prepare(QStringLiteral(
        "DELETE FROM sleep_staged_change WHERE session_id=? AND status='Prepared'"));
    query.addBindValue(sessionId);
    return query.exec();
}

int MemoryStore::preparedSleepChangeCount(const QString& sessionId) const {
    if (!m_repository || !m_repository->isOpen()) return 0;
    QSqlQuery query(QSqlDatabase::database(m_repository->connectionName()));
    query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM sleep_staged_change "
        "WHERE session_id=? AND status='Prepared'"));
    query.addBindValue(sessionId);
    return query.exec() && query.next() ? query.value(0).toInt() : 0;
}

bool MemoryStore::hasSleepChange(const QString& changeId,
                                 const QString& payloadHash) const {
    if (!m_repository || !m_repository->isOpen()) return false;
    QSqlQuery query(QSqlDatabase::database(m_repository->connectionName()));
    query.prepare(QStringLiteral(
        "SELECT 1 FROM sleep_staged_change WHERE change_id=? AND payload_hash=? LIMIT 1"));
    query.addBindValue(changeId);
    query.addBindValue(payloadHash);
    return query.exec() && query.next();
}

bool MemoryStore::isSleepChangeFinalized(const QString& changeId,
                                         const QString& payloadHash) const {
    if (!m_repository || !m_repository->isOpen()) return false;
    QSqlQuery query(QSqlDatabase::database(m_repository->connectionName()));
    query.prepare(QStringLiteral(
        "SELECT 1 FROM sleep_staged_change "
        "WHERE change_id=? AND payload_hash=? AND status='Finalized' LIMIT 1"));
    query.addBindValue(changeId);
    query.addBindValue(payloadHash);
    return query.exec() && query.next();
}

bool MemoryStore::finalizeSleepChange(const QString& changeId,
                                      const QString& payloadHash) {
    if (!m_repository || !m_repository->isOpen()
        || changeId.trimmed().isEmpty() || payloadHash.trimmed().isEmpty()) {
        return false;
    }
    QSqlDatabase database = QSqlDatabase::database(m_repository->connectionName());
    QSqlQuery update(database);
    update.prepare(QStringLiteral(
        "UPDATE sleep_staged_change SET status='Finalized',finalized_at=? "
        "WHERE change_id=? AND payload_hash=? AND status IN ('Prepared','Finalized')"));
    update.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    update.addBindValue(changeId);
    update.addBindValue(payloadHash);
    if (!update.exec()) return false;
    QSqlQuery verify(database);
    verify.prepare(QStringLiteral(
        "SELECT 1 FROM sleep_staged_change "
        "WHERE change_id=? AND payload_hash=? AND status='Finalized' LIMIT 1"));
    verify.addBindValue(changeId);
    verify.addBindValue(payloadHash);
    return verify.exec() && verify.next();
}

bool MemoryStore::removeEntryById(const QString& id) {
    if (!m_repository || !m_repository->isOpen() || id.isEmpty()) return false;
    if (!m_repository->removeById(id)) return false;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].id == id) {
            m_entries.removeAt(i);
            break;
        }
    }
    return true;
}

bool MemoryStore::load(QString* errorMessage) {
    QString dbError;
    if (!m_repository->open(m_databasePath, &dbError)) {
        if (errorMessage) *errorMessage = QStringLiteral("SQLite open failed: %1").arg(dbError);
        return false;
    }

    m_relationGraph.setConnectionName(m_repository->connectionName());
    m_tagCooccurrenceGraph.setConnectionName(m_repository->connectionName());

    QList<MemoryEntry> existing = m_repository->loadAll();

    if (existing.isEmpty() && QFile::exists(m_memoryFilePath)) {
        if (!importLegacyJson(m_memoryFilePath, errorMessage)) return false;
        existing = m_repository->loadAll();
    }

    m_entries = existing;
    return true;
}

bool MemoryStore::importLegacyJson(const QString& jsonPath, QString* errorMessage) {
    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        if (errorMessage) *errorMessage = QStringLiteral("legacy memory JSON must be an array");
        return false;
    }

    QList<MemoryEntry> entries;
    QSet<QString> ids;
    for (const QJsonValue& value : document.array()) {
        if (!value.isObject()) continue;
        MemoryEntry entry = MemoryEntry::fromJson(value.toObject());
        if (entry.id.isEmpty()) continue;
        if (ids.contains(entry.id)) {
            if (errorMessage) *errorMessage = QStringLiteral("legacy memory JSON contains duplicate ids");
            return false;
        }
        ids.insert(entry.id);
        entries.append(std::move(entry));
    }

    if (!m_repository->isOpen()) {
        QString databaseError;
        if (!m_repository->open(m_databasePath, &databaseError)) {
            if (errorMessage) *errorMessage = databaseError;
            return false;
        }
        m_relationGraph.setConnectionName(m_repository->connectionName());
        m_tagCooccurrenceGraph.setConnectionName(m_repository->connectionName());
    }
    if (!m_repository->loadAll().isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("legacy JSON target database is not empty");
        return false;
    }
    if (!m_repository->beginTransaction()) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to begin legacy JSON import transaction");
        return false;
    }
    for (const MemoryEntry& entry : entries) {
        if (!m_repository->insert(entry)) {
            m_repository->rollbackTransaction();
            if (errorMessage) *errorMessage = QStringLiteral("failed to import legacy memory entry");
            return false;
        }
    }
    if (!m_repository->commitTransaction()) {
        m_repository->rollbackTransaction();
        if (errorMessage) *errorMessage = QStringLiteral("failed to commit legacy JSON import");
        return false;
    }
    m_entries = m_repository->loadAll();
    return true;
}

bool MemoryStore::save(QString* errorMessage) const {
    const QFileInfo info(m_memoryFilePath);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().path())) {
        if (errorMessage) *errorMessage = "failed to create memory directory";
        return false;
    }

    QJsonArray array;
    for (const MemoryEntry& entry : m_entries) {
        array.append(entry.toJson());
    }

    QSaveFile file(m_memoryFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }

    const QByteArray payload = QJsonDocument(array).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size() || !file.commit()) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    return true;
}

MemoryEntry MemoryStore::add(MemoryType type,
                             const QString& key,
                             const QJsonValue& value,
                             const QStringList& tags) {
    MemoryEntry entry;
    entry.type = type;
    entry.key = key;
    entry.value = value;
    entry.tags = tags;
    entry.summary = fallbackSummary(entry);
    entry.status = MemoryStatus::Active;
    entry.privacyLevel = PrivacyLevel::Public;
    entry.source = tags.contains("assistant") ? QString("assistant_inferred") : QString("tool_result");
    entry.confidence = type == MemoryType::ShortTerm || type == MemoryType::Working ? 0.4 : 0.75;
    entry.importance = type == MemoryType::ShortTerm || type == MemoryType::Working ? 0.2 : 0.5;
    entry.strength = entry.importance;
    return addEntry(entry);
}

MemoryEntry MemoryStore::addEntry(const MemoryEntry& entry) {
    MemoryEntry stored = entry;
    if (stored.id.trimmed().isEmpty()) {
        stored.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!stored.createdAt.isValid()) {
        stored.createdAt = QDateTime::currentDateTimeUtc();
    }
    if (!stored.updatedAt.isValid()) {
        stored.updatedAt = stored.createdAt;
    }
    if (stored.summary.trimmed().isEmpty()) {
        stored.summary = fallbackSummary(stored);
    }
    if (stored.source.trimmed().isEmpty()) {
        stored.source = "user_explicit";
    }
    if (stored.confidence <= 0.0) {
        stored.confidence = 0.75;
    }
    if (stored.importance <= 0.0) {
        stored.importance = 0.5;
    }
    if (stored.strength <= 0.0) {
        stored.strength = stored.importance;
    }
    if (stored.partition.trimmed().isEmpty()) {
        stored.partition = partitionToString(partitionForType(stored.type));  // 派生自 type
    }
    // Core 类型（用户身份等承载性事实）落入 Semantic 分区（可被自适应遗忘），
    // 但写入时设 importance 下限，靠自适应（importance×access 拉伸半衰期）保证近不朽，
    // 同时仍可被更高优先级事实 supersedes。不设独立永不遗忘分区。
    if (stored.type == MemoryType::Core && stored.importance < 0.8) {
        stored.importance = 0.8;
        if (stored.strength < 0.8) stored.strength = 0.8;
    }

    if (!persistEntry(stored)) {
        return {};
    }
    m_entries.append(stored);
    return stored;
}

bool MemoryStore::updateEntryById(const MemoryEntry& entry) {
    if (entry.id.trimmed().isEmpty()) {
        return false;
    }

    for (MemoryEntry& existing : m_entries) {
        if (existing.id != entry.id) {
            continue;
        }

        MemoryEntry stored = entry;
        if (!stored.createdAt.isValid()) {
            stored.createdAt = existing.createdAt.isValid()
                ? existing.createdAt
                : QDateTime::currentDateTimeUtc();
        }
        stored.updatedAt = QDateTime::currentDateTimeUtc();
        if (stored.summary.trimmed().isEmpty()) {
            stored.summary = fallbackSummary(stored);
        }

        if (m_repository && m_repository->isOpen()) {
            if (!m_repository->update(stored)) {
                return false;
            }
        }
        existing = stored;
        return true;
    }

    return false;
}

bool MemoryStore::updateStatusById(const QString& id,
                                   MemoryStatus status,
                                   const QJsonObject& payloadPatch) {
    if (id.trimmed().isEmpty()) {
        return false;
    }

    for (MemoryEntry& entry : m_entries) {
        if (entry.id != id) {
            continue;
        }

        MemoryEntry updated = entry;
        updated.status = status;
        updated.updatedAt = QDateTime::currentDateTimeUtc();
        for (auto it = payloadPatch.constBegin(); it != payloadPatch.constEnd(); ++it) {
            updated.payload[it.key()] = it.value();
        }

        if (!persistStatusUpdate(id, status, payloadPatch)) {
            return false;
        }
        entry = updated;
        return true;
    }

    return false;
}

bool MemoryStore::updateStatusByKey(MemoryType type,
                                    const QString& key,
                                    MemoryStatus status,
                                    const QJsonObject& payloadPatch) {
    bool changed = false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (MemoryEntry& entry : m_entries) {
        if (entry.type != type || entry.key != key || entry.status == status) {
            continue;
        }
        MemoryEntry updated = entry;
        updated.status = status;
        updated.updatedAt = now;
        for (auto it = payloadPatch.constBegin(); it != payloadPatch.constEnd(); ++it) {
            updated.payload[it.key()] = it.value();
        }
        if (!persistStatusUpdate(entry.id, status, payloadPatch)) {
            continue;
        }
        entry = updated;
        changed = true;
    }
    return changed;
}

bool MemoryStore::updateTaskShadowStatus(const QString& linkedTaskId,
                                         MemoryStatus status,
                                         const QJsonObject& payloadPatch) {
    bool changed = false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (MemoryEntry& entry : m_entries) {
        if (entry.type != MemoryType::TaskShadow) continue;
        if (entry.payload.value("linked_task_id").toString() != linkedTaskId) continue;
        MemoryEntry updated = entry;
        updated.status = status;
        updated.updatedAt = now;
        for (auto it = payloadPatch.constBegin(); it != payloadPatch.constEnd(); ++it) {
            updated.payload[it.key()] = it.value();
        }
        if (!persistStatusUpdate(entry.id, status, payloadPatch)) {
            continue;
        }
        entry = updated;
        changed = true;
    }
    return changed;
}

QList<MemoryEntry> MemoryStore::recent(MemoryType type, int limit) const {
    QList<MemoryEntry> result;
    for (auto it = m_entries.crbegin(); it != m_entries.crend() && result.size() < limit; ++it) {
        if (it->type == type && it->status != MemoryStatus::Deleted) {
            result.append(*it);
        }
    }
    return result;
}

QList<MemoryEntry> MemoryStore::findByTag(const QString& tag, int limit) const {
    QList<MemoryEntry> result;
    for (auto it = m_entries.crbegin(); it != m_entries.crend() && result.size() < limit; ++it) {
        if (it->status != MemoryStatus::Deleted && it->tags.contains(tag)) {
            result.append(*it);
        }
    }
    return result;
}

QStringList MemoryStore::summaryForContext(int limit) const {
    QStringList result;
    for (auto it = m_entries.crbegin(); it != m_entries.crend() && result.size() < limit; ++it) {
        if (!shouldInjectIntoContext(*it)) continue;

        const QString summaryText = fallbackSummary(*it);
        if (summaryText.trimmed().isEmpty()) continue;

        QStringList labels;
        labels.append(memoryTypeToString(it->type));
        const QString confidence = confidenceLabel(it->confidence);
        if (!confidence.isEmpty()) labels.append(confidence);
        if (!it->scope.trimmed().isEmpty()) labels.append(it->scope.trimmed());

        result.append(QString("[%1] %2").arg(labels.join("/"), summaryText));
    }
    return result;
}

void MemoryStore::clear() {
    if (m_repository && m_repository->isOpen() && !m_repository->clear()) {
        return;
    }
    m_entries.clear();
}

bool MemoryStore::persistEntry(const MemoryEntry& entry) {
    if (m_repository && m_repository->isOpen()) {
        return m_repository->insert(entry);
    }
    return true;
}

bool MemoryStore::persistStatusUpdate(const QString& id,
                                      MemoryStatus status,
                                      const QJsonObject& payloadPatch) {
    if (m_repository && m_repository->isOpen()) {
        return m_repository->updateStatus(id, status, payloadPatch);
    }
    return true;
}

MemoryEntry* MemoryStore::findById(const QString& id) {
    for (MemoryEntry& entry : m_entries) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

const MemoryEntry* MemoryStore::findById(const QString& id) const {
    for (const MemoryEntry& entry : m_entries) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}
