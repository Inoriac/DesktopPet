#include "sqlite_private_psyche_repository.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace {

QString newConnectionName() {
    return QStringLiteral("private_psyche_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
}

DomainError sqlError(const QString& message, const QSqlError& error = {}) {
    QJsonObject details;
    if (error.isValid()) details.insert(QStringLiteral("driver"), error.driverText());
    return domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"), message, details);
}

QJsonObject parseObject(const QString& value) {
    const QJsonDocument document = QJsonDocument::fromJson(value.toUtf8());
    return document.isObject() ? document.object() : QJsonObject{};
}

QString metadataCursor(const DiaryMetadata& metadata) {
    const QJsonObject object{
        {QStringLiteral("localDate"),
         metadata.localDate.toString(Qt::ISODate)},
        {QStringLiteral("entryId"), metadata.entryId}
    };
    return QString::fromLatin1(
        QJsonDocument(object).toJson(QJsonDocument::Compact).toBase64(
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

Result<QPair<QDate, QString>, DomainError> parseMetadataCursor(
    const QString& cursor) {
    if (cursor.isEmpty()) {
        return Result<QPair<QDate, QString>, DomainError>::success({{}, {}});
    }
    const QByteArray decoded = QByteArray::fromBase64(
        cursor.toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    const QJsonDocument document = QJsonDocument::fromJson(decoded);
    if (!document.isObject()) {
        return Result<QPair<QDate, QString>, DomainError>::failure(
            domainError(QStringLiteral("DIARY_QUERY_INVALID"),
                        QStringLiteral("diary cursor is invalid")));
    }
    const QJsonObject object = document.object();
    const QDate date = QDate::fromString(
        object.value(QStringLiteral("localDate")).toString(), Qt::ISODate);
    const QString entryId = object.value(QStringLiteral("entryId")).toString();
    if (!date.isValid() || entryId.trimmed().isEmpty()) {
        return Result<QPair<QDate, QString>, DomainError>::failure(
            domainError(QStringLiteral("DIARY_QUERY_INVALID"),
                        QStringLiteral("diary cursor fields are invalid")));
    }
    return Result<QPair<QDate, QString>, DomainError>::success({date, entryId});
}

StoredPrivateRecord recordFromQuery(QSqlQuery& query,
                                    const QString& recordType) {
    StoredPrivateRecord record;
    record.recordId = query.value(QStringLiteral("record_id")).toString();
    record.profileId = query.value(QStringLiteral("profile_id")).toString();
    record.recordType = recordType;
    record.localDate = QDate::fromString(
        query.value(QStringLiteral("local_date")).toString(), Qt::ISODate);
    record.sourceEventId = query.value(QStringLiteral("source_event_id")).toString();
    record.index = parseObject(query.value(QStringLiteral("index_json")).toString());
    record.sourceCutoffSequence =
        query.value(QStringLiteral("source_cutoff_sequence")).toLongLong();
    record.encrypted.schemaVersion =
        query.value(QStringLiteral("schema_version")).toInt();
    record.encrypted.keyVersion = query.value(QStringLiteral("key_version")).toInt();
    record.encrypted.nonce = query.value(QStringLiteral("nonce")).toByteArray();
    record.encrypted.ciphertext = query.value(QStringLiteral("ciphertext")).toByteArray();
    record.createdAt = QDateTime::fromString(
        query.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
    return record;
}

} // namespace

SqlitePrivatePsycheRepository::SqlitePrivatePsycheRepository()
    : m_connectionName(newConnectionName()) {}

SqlitePrivatePsycheRepository::~SqlitePrivatePsycheRepository() {
    close();
}

Result<void, DomainError> SqlitePrivatePsycheRepository::open(
    const QString& databasePath) {
    if (databasePath.trimmed().isEmpty()) {
        return Result<void, DomainError>::failure(
            sqlError(QStringLiteral("private database path is empty")));
    }
    close();
    m_connectionName = newConnectionName();
    QSqlDatabase database = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), m_connectionName);
    database.setDatabaseName(databasePath);
    if (!database.open()) {
        const DomainError error = sqlError(
            QStringLiteral("failed to open private database"), database.lastError());
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        return Result<void, DomainError>::failure(error);
    }
    QSqlQuery pragma(database);
    if (!pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"))
        || !pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"))
        || !pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"))) {
        const DomainError error = sqlError(
            QStringLiteral("failed to configure private database"), pragma.lastError());
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        return Result<void, DomainError>::failure(error);
    }
    const auto migrated = migrateSchema(database);
    if (!migrated.isOk()) {
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        return migrated;
    }
    m_databasePath = databasePath;
    return Result<void, DomainError>::success();
}

void SqlitePrivatePsycheRepository::close() {
    if (m_connectionName.isEmpty() || !QSqlDatabase::contains(m_connectionName)) {
        m_databasePath.clear();
        return;
    }
    {
        QSqlDatabase database = QSqlDatabase::database(m_connectionName, false);
        if (database.isValid()) database.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
    m_databasePath.clear();
}

bool SqlitePrivatePsycheRepository::isOpen() const {
    return QSqlDatabase::contains(m_connectionName)
        && QSqlDatabase::database(m_connectionName, false).isOpen();
}

Result<void, DomainError> SqlitePrivatePsycheRepository::migrateSchema(
    QSqlDatabase& database) {
    QSqlQuery versionQuery(database);
    if (!versionQuery.exec(QStringLiteral("PRAGMA user_version"))
        || !versionQuery.next()) {
        return Result<void, DomainError>::failure(
            sqlError(QStringLiteral("failed to read private schema version"),
                     versionQuery.lastError()));
    }
    const int version = versionQuery.value(0).toInt();
    if (version < 0 || version > 1) {
        return Result<void, DomainError>::failure(
            sqlError(QStringLiteral("private schema version is newer than supported")));
    }
    if (!database.transaction()) {
        return Result<void, DomainError>::failure(
            sqlError(QStringLiteral("failed to begin private schema migration"),
                     database.lastError()));
    }
    QSqlQuery query(database);
    const QStringList statements{
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS inner_thought ("
            "thought_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL,"
            "source_event_id TEXT,"
            "key_version INTEGER NOT NULL, nonce BLOB NOT NULL,"
            "ciphertext BLOB NOT NULL, index_json TEXT NOT NULL,"
            "occurred_at TEXT NOT NULL, created_at TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_inner_thought_profile_created "
            "ON inner_thought(profile_id, created_at)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS diary_entry ("
            "entry_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL,"
            "local_date TEXT NOT NULL,"
            "key_version INTEGER NOT NULL, nonce BLOB NOT NULL,"
            "ciphertext BLOB NOT NULL, index_json TEXT NOT NULL,"
            "ciphertext_hash TEXT NOT NULL, source_cutoff_sequence INTEGER NOT NULL,"
            "created_at TEXT NOT NULL, UNIQUE(profile_id, local_date))"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS sleep_staged_change ("
            "session_id TEXT NOT NULL, change_id TEXT NOT NULL,"
            "target_type TEXT NOT NULL,"
            "key_version INTEGER NOT NULL, nonce BLOB NOT NULL,"
            "ciphertext BLOB NOT NULL, ciphertext_hash TEXT NOT NULL,"
            "created_at TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'Prepared',"
            "finalized_at TEXT,"
            "PRIMARY KEY(session_id, change_id))"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_private_sleep_staged_status "
            "ON sleep_staged_change(session_id, status)")
    };
    for (const QString& sql : statements) {
        if (!query.exec(sql)) {
            database.rollback();
            return Result<void, DomainError>::failure(
                sqlError(QStringLiteral("private schema migration failed"),
                         query.lastError()));
        }
    }
    if (version == 0
        && !query.exec(QStringLiteral("PRAGMA user_version=1"))) {
        database.rollback();
        return Result<void, DomainError>::failure(
            sqlError(QStringLiteral("failed to set private schema version"),
                     query.lastError()));
    }
    if (!database.commit()) {
        database.rollback();
        return Result<void, DomainError>::failure(
            sqlError(QStringLiteral("private schema commit failed"),
                     database.lastError()));
    }
    return Result<void, DomainError>::success();
}

Result<void, DomainError> SqlitePrivatePsycheRepository::saveInnerThought(
    const InnerThoughtSummary& summary,
    const EncryptedPrivatePayload& encrypted,
    const QJsonObject& index) {
    if (!isOpen()) {
        return Result<void, DomainError>::failure(
            sqlError(QStringLiteral("private repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO inner_thought("
        "thought_id, profile_id, source_event_id, key_version, nonce, ciphertext,"
        "index_json, occurred_at, created_at) VALUES(?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(summary.entryId);
    query.addBindValue(summary.profileId);
    query.addBindValue(summary.sourceEventId);
    query.addBindValue(encrypted.keyVersion);
    query.addBindValue(encrypted.nonce);
    query.addBindValue(encrypted.ciphertext);
    query.addBindValue(QString::fromUtf8(
        QJsonDocument(index).toJson(QJsonDocument::Compact)));
    query.addBindValue(summary.createdAt.toString(Qt::ISODateWithMs));
    query.addBindValue(summary.createdAt.toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        return Result<void, DomainError>::failure(
            sqlError(QStringLiteral("failed to store inner thought"), query.lastError()));
    }
    return Result<void, DomainError>::success();
}

Result<std::optional<StoredPrivateRecord>, DomainError>
SqlitePrivatePsycheRepository::innerThought(const QString& entryId) const {
    return readRecord(QStringLiteral("inner_thought"), QStringLiteral("thought_id"),
                      QStringLiteral("inner_thought"), entryId);
}

Result<void, DomainError> SqlitePrivatePsycheRepository::stageDiary(
    const QString& sessionId,
    const DiaryEntry& entry,
    const EncryptedPrivatePayload& encrypted) {
    if (!isOpen() || sessionId.trimmed().isEmpty()) {
        return Result<void, DomainError>::failure(
            sqlError(QStringLiteral("private diary staging is unavailable")));
    }
    const QString ciphertextHash = QString::fromLatin1(QCryptographicHash::hash(
        encrypted.ciphertext, QCryptographicHash::Sha256).toHex());
    QSqlDatabase database = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO sleep_staged_change("
        "session_id, change_id, target_type, key_version, nonce, ciphertext,"
        "ciphertext_hash, created_at, status) VALUES(?,?,?,?,?,?,?,?,'Prepared')"));
    query.addBindValue(sessionId);
    query.addBindValue(entry.entryId);
    query.addBindValue(QStringLiteral("diary_entry"));
    query.addBindValue(encrypted.keyVersion);
    query.addBindValue(encrypted.nonce);
    query.addBindValue(encrypted.ciphertext);
    query.addBindValue(ciphertextHash);
    query.addBindValue(entry.createdAt.toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        return Result<void, DomainError>::failure(
            sqlError(QStringLiteral("failed to stage diary"), query.lastError()));
    }
    QSqlQuery verify(database);
    verify.prepare(QStringLiteral(
        "SELECT target_type,key_version,nonce,ciphertext,ciphertext_hash,status "
        "FROM sleep_staged_change WHERE session_id=? AND change_id=?"));
    verify.addBindValue(sessionId);
    verify.addBindValue(entry.entryId);
    if (!verify.exec() || !verify.next()
        || verify.value(0).toString() != QLatin1String("diary_entry")
        || verify.value(1).toInt() != encrypted.keyVersion
        || verify.value(2).toByteArray() != encrypted.nonce
        || verify.value(3).toByteArray() != encrypted.ciphertext
        || verify.value(4).toString() != ciphertextHash
        || verify.value(5).toString() != QLatin1String("Prepared")) {
        return Result<void, DomainError>::failure(
            sqlError(QStringLiteral("existing diary staging row is inconsistent"),
                     verify.lastError()));
    }
    return Result<void, DomainError>::success();
}

Result<std::optional<StoredPrivateRecord>, DomainError>
SqlitePrivatePsycheRepository::committedDiaryForDate(
    const QString& profileId, const QDate& localDate) const {
    if (!isOpen()) {
        return Result<std::optional<StoredPrivateRecord>, DomainError>::failure(
            sqlError(QStringLiteral("private repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT entry_id AS record_id, profile_id, local_date,"
        "NULL AS source_event_id, 1 AS schema_version, key_version, nonce, ciphertext,"
        "index_json, source_cutoff_sequence, created_at FROM diary_entry "
        "WHERE profile_id=? AND local_date=?"));
    query.addBindValue(profileId);
    query.addBindValue(localDate.toString(Qt::ISODate));
    if (!query.exec()) {
        return Result<std::optional<StoredPrivateRecord>, DomainError>::failure(
            sqlError(QStringLiteral("failed to query diary date"), query.lastError()));
    }
    if (!query.next()) {
        return Result<std::optional<StoredPrivateRecord>, DomainError>::success(std::nullopt);
    }
    return Result<std::optional<StoredPrivateRecord>, DomainError>::success(
        recordFromQuery(query, QStringLiteral("diary_entry")));
}

Result<std::optional<StoredPrivateRecord>, DomainError>
SqlitePrivatePsycheRepository::diary(const QString& entryId) const {
    return readRecord(QStringLiteral("diary_entry"), QStringLiteral("entry_id"),
                      QStringLiteral("diary_entry"), entryId);
}

Result<DiaryPage, DomainError>
SqlitePrivatePsycheRepository::diaryMetadataPage(
    const QString& profileId,
    const DiaryListQuery& query) const {
    if (!isOpen()) {
        return Result<DiaryPage, DomainError>::failure(
            sqlError(QStringLiteral("private repository is closed")));
    }
    if (profileId.trimmed().isEmpty()
        || (query.from.isValid() && query.to.isValid() && query.from > query.to)) {
        return Result<DiaryPage, DomainError>::failure(
            domainError(QStringLiteral("DIARY_QUERY_INVALID"),
                        QStringLiteral("diary metadata query is invalid")));
    }
    const auto cursor = parseMetadataCursor(query.cursor);
    if (!cursor.isOk()) {
        return Result<DiaryPage, DomainError>::failure(cursor.error());
    }

    QStringList predicates{QStringLiteral("profile_id=?")};
    QVariantList bindings{profileId};
    if (query.from.isValid()) {
        predicates.append(QStringLiteral("local_date>=?"));
        bindings.append(query.from.toString(Qt::ISODate));
    }
    if (query.to.isValid()) {
        predicates.append(QStringLiteral("local_date<=?"));
        bindings.append(query.to.toString(Qt::ISODate));
    }
    if (cursor.value().first.isValid()) {
        predicates.append(QStringLiteral(
            "(local_date<? OR (local_date=? AND entry_id<?))"));
        bindings.append(cursor.value().first.toString(Qt::ISODate));
        bindings.append(cursor.value().first.toString(Qt::ISODate));
        bindings.append(cursor.value().second);
    }
    const int limit = std::clamp(query.limit, 1, 100);
    QSqlQuery sql(QSqlDatabase::database(m_connectionName));
    sql.prepare(QStringLiteral(
        "SELECT entry_id,local_date,index_json,created_at FROM diary_entry "
        "WHERE %1 ORDER BY local_date DESC,entry_id DESC LIMIT ?")
                    .arg(predicates.join(QStringLiteral(" AND "))));
    for (const QVariant& binding : bindings) sql.addBindValue(binding);
    sql.addBindValue(limit + 1);
    if (!sql.exec()) {
        return Result<DiaryPage, DomainError>::failure(
            sqlError(QStringLiteral("failed to list diary metadata"), sql.lastError()));
    }

    DiaryPage page;
    while (page.entries.size() < limit && sql.next()) {
        DiaryMetadata metadata;
        metadata.entryId = sql.value(QStringLiteral("entry_id")).toString();
        metadata.localDate = QDate::fromString(
            sql.value(QStringLiteral("local_date")).toString(), Qt::ISODate);
        metadata.index = parseObject(
            sql.value(QStringLiteral("index_json")).toString());
        metadata.createdAt = QDateTime::fromString(
            sql.value(QStringLiteral("created_at")).toString(),
            Qt::ISODateWithMs);
        if (metadata.entryId.isEmpty() || !metadata.localDate.isValid()) {
            return Result<DiaryPage, DomainError>::failure(
                sqlError(QStringLiteral("stored diary metadata is invalid")));
        }
        page.entries.append(std::move(metadata));
    }
    const bool hasMore = sql.next();
    if (hasMore && !page.entries.isEmpty()) {
        page.nextCursor = metadataCursor(page.entries.last());
    }
    return Result<DiaryPage, DomainError>::success(std::move(page));
}

Result<std::optional<StoredPrivateRecord>, DomainError>
SqlitePrivatePsycheRepository::readRecord(
    const QString& table,
    const QString& idColumn,
    const QString& recordType,
    const QString& recordId) const {
    if (!isOpen()) {
        return Result<std::optional<StoredPrivateRecord>, DomainError>::failure(
            sqlError(QStringLiteral("private repository is closed")));
    }
    if (table != QLatin1String("inner_thought")
        && table != QLatin1String("diary_entry")) {
        return Result<std::optional<StoredPrivateRecord>, DomainError>::failure(
            sqlError(QStringLiteral("private record type is invalid")));
    }
    const QString localDate = table == QLatin1String("diary_entry")
        ? QStringLiteral("local_date") : QStringLiteral("NULL AS local_date");
    const QString sourceEvent = table == QLatin1String("inner_thought")
        ? QStringLiteral("source_event_id") : QStringLiteral("NULL AS source_event_id");
    const QString sourceCutoff = table == QLatin1String("diary_entry")
        ? QStringLiteral("source_cutoff_sequence")
        : QStringLiteral("0 AS source_cutoff_sequence");
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT %1 AS record_id, profile_id, %2, %3, 1 AS schema_version, key_version,"
        "nonce, ciphertext, index_json, %4, created_at FROM %5 WHERE %1=?")
                      .arg(idColumn, localDate, sourceEvent, sourceCutoff, table));
    query.addBindValue(recordId);
    if (!query.exec()) {
        return Result<std::optional<StoredPrivateRecord>, DomainError>::failure(
            sqlError(QStringLiteral("failed to read private record"), query.lastError()));
    }
    if (!query.next()) {
        return Result<std::optional<StoredPrivateRecord>, DomainError>::success(std::nullopt);
    }
    return Result<std::optional<StoredPrivateRecord>, DomainError>::success(
        recordFromQuery(query, recordType));
}

Result<QList<StoredPrivateRecord>, DomainError>
SqlitePrivatePsycheRepository::preparedDiaries(
    const QString& sessionId,
    const QString& profileId) const {
    if (!isOpen()) {
        return Result<QList<StoredPrivateRecord>, DomainError>::failure(
            sqlError(QStringLiteral("private repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT change_id AS record_id, ? AS profile_id, NULL AS local_date,"
        "NULL AS source_event_id, 1 AS schema_version, key_version, nonce, ciphertext,"
        "ciphertext_hash, '{}' AS index_json, 0 AS source_cutoff_sequence, created_at "
        "FROM sleep_staged_change WHERE session_id=? AND target_type='diary_entry' "
        "AND status IN ('Prepared','Finalized') ORDER BY created_at, change_id"));
    query.addBindValue(profileId);
    query.addBindValue(sessionId);
    if (!query.exec()) {
        return Result<QList<StoredPrivateRecord>, DomainError>::failure(
            sqlError(QStringLiteral("failed to read prepared diaries"), query.lastError()));
    }
    QList<StoredPrivateRecord> records;
    while (query.next()) {
        StoredPrivateRecord record = recordFromQuery(
            query, QStringLiteral("diary_entry"));
        const QString actualHash = QString::fromLatin1(QCryptographicHash::hash(
            record.encrypted.ciphertext, QCryptographicHash::Sha256).toHex());
        if (actualHash != query.value(QStringLiteral("ciphertext_hash")).toString()) {
            return Result<QList<StoredPrivateRecord>, DomainError>::failure(
                sqlError(QStringLiteral("prepared diary ciphertext hash mismatch")));
        }
        records.append(std::move(record));
    }
    return Result<QList<StoredPrivateRecord>, DomainError>::success(records);
}

Result<QString, DomainError> SqlitePrivatePsycheRepository::finalizeDiary(
    const QString& sessionId,
    const DiaryEntry& entry,
    const EncryptedPrivatePayload& encrypted) {
    if (!isOpen()) {
        return Result<QString, DomainError>::failure(
            sqlError(QStringLiteral("private repository is closed")));
    }
    QSqlDatabase database = QSqlDatabase::database(m_connectionName);
    if (!database.transaction()) {
        return Result<QString, DomainError>::failure(
            sqlError(QStringLiteral("failed to begin diary finalize"), database.lastError()));
    }
    const QString ciphertextHash = QString::fromLatin1(QCryptographicHash::hash(
        encrypted.ciphertext, QCryptographicHash::Sha256).toHex());
    QSqlQuery staged(database);
    staged.prepare(QStringLiteral(
        "SELECT target_type,key_version,nonce,ciphertext,ciphertext_hash,status "
        "FROM sleep_staged_change WHERE session_id=? AND change_id=?"));
    staged.addBindValue(sessionId);
    staged.addBindValue(entry.entryId);
    if (!staged.exec() || !staged.next()) {
        database.rollback();
        return Result<QString, DomainError>::failure(
            sqlError(QStringLiteral("diary staging row is missing"), staged.lastError()));
    }
    const QByteArray stagedCiphertext = staged.value(3).toByteArray();
    const QString stagedActualHash = QString::fromLatin1(QCryptographicHash::hash(
        stagedCiphertext, QCryptographicHash::Sha256).toHex());
    if (staged.value(0).toString() != QLatin1String("diary_entry")
        || staged.value(1).toInt() != encrypted.keyVersion
        || staged.value(2).toByteArray() != encrypted.nonce
        || stagedCiphertext != encrypted.ciphertext
        || staged.value(4).toString() != ciphertextHash
        || stagedActualHash != ciphertextHash
        || (staged.value(5).toString() != QLatin1String("Prepared")
            && staged.value(5).toString() != QLatin1String("Finalized"))) {
        database.rollback();
        return Result<QString, DomainError>::failure(
            sqlError(QStringLiteral("diary staging row failed integrity validation")));
    }
    const QString indexJson = QString::fromUtf8(
        QJsonDocument(entry.index).toJson(QJsonDocument::Compact));
    const QString createdAt = entry.createdAt.toString(Qt::ISODateWithMs);
    QSqlQuery existing(database);
    existing.prepare(QStringLiteral(
        "SELECT entry_id,key_version,nonce,ciphertext,index_json,ciphertext_hash,"
        "source_cutoff_sequence,created_at FROM diary_entry "
        "WHERE profile_id=? AND local_date=?"));
    existing.addBindValue(entry.profileId);
    existing.addBindValue(entry.localDate.toString(Qt::ISODate));
    if (!existing.exec()) {
        database.rollback();
        return Result<QString, DomainError>::failure(
            sqlError(QStringLiteral("failed to check diary uniqueness"), existing.lastError()));
    }
    QString canonicalEntryId;
    if (existing.next()) {
        canonicalEntryId = existing.value(0).toString();
        if (canonicalEntryId != entry.entryId
            || existing.value(1).toInt() != encrypted.keyVersion
            || existing.value(2).toByteArray() != encrypted.nonce
            || existing.value(3).toByteArray() != encrypted.ciphertext
            || existing.value(4).toString() != indexJson
            || existing.value(5).toString() != ciphertextHash
            || existing.value(6).toLongLong() != entry.sourceCutoffSequence
            || existing.value(7).toString() != createdAt) {
            database.rollback();
            return Result<QString, DomainError>::failure(
                sqlError(QStringLiteral(
                    "finalized diary does not match its staging row")));
        }
    } else {
        if (staged.value(5).toString() == QLatin1String("Finalized")) {
            database.rollback();
            return Result<QString, DomainError>::failure(
                sqlError(QStringLiteral(
                    "finalized diary staging row has no formal diary")));
        }
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral(
            "INSERT INTO diary_entry(entry_id, profile_id, local_date, ciphertext, nonce,"
            "key_version, index_json, ciphertext_hash, source_cutoff_sequence, created_at)"
            " VALUES(?,?,?,?,?,?,?,?,?,?)"));
        insert.addBindValue(entry.entryId);
        insert.addBindValue(entry.profileId);
        insert.addBindValue(entry.localDate.toString(Qt::ISODate));
        insert.addBindValue(encrypted.ciphertext);
        insert.addBindValue(encrypted.nonce);
        insert.addBindValue(encrypted.keyVersion);
        insert.addBindValue(indexJson);
        insert.addBindValue(ciphertextHash);
        insert.addBindValue(entry.sourceCutoffSequence);
        insert.addBindValue(createdAt);
        if (!insert.exec()) {
            database.rollback();
            return Result<QString, DomainError>::failure(
                sqlError(QStringLiteral("failed to materialize diary"), insert.lastError()));
        }
        canonicalEntryId = entry.entryId;
    }
    QSqlQuery update(database);
    update.prepare(QStringLiteral(
        "UPDATE sleep_staged_change SET status='Finalized', finalized_at=? "
        "WHERE session_id=? AND change_id=? AND status IN ('Prepared','Finalized')"));
    update.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    update.addBindValue(sessionId);
    update.addBindValue(entry.entryId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        database.rollback();
        return Result<QString, DomainError>::failure(
            sqlError(QStringLiteral("diary staging row was not finalized"),
                     update.lastError()));
    }
    QSqlQuery verify(database);
    verify.prepare(QStringLiteral(
        "SELECT status FROM sleep_staged_change WHERE session_id=? AND change_id=?"));
    verify.addBindValue(sessionId);
    verify.addBindValue(entry.entryId);
    if (!verify.exec() || !verify.next()
        || verify.value(0).toString() != QLatin1String("Finalized")
        || !database.commit()) {
        database.rollback();
        return Result<QString, DomainError>::failure(
            sqlError(QStringLiteral("failed to complete diary finalize"),
                     verify.lastError().isValid() ? verify.lastError()
                                                  : database.lastError()));
    }
    return Result<QString, DomainError>::success(canonicalEntryId);
}

Result<void, DomainError> SqlitePrivatePsycheRepository::abortSession(
    const QString& sessionId) {
    if (!isOpen()) return Result<void, DomainError>::success();
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "DELETE FROM sleep_staged_change WHERE session_id=? AND status='Prepared'"));
    query.addBindValue(sessionId);
    if (!query.exec()) {
        return Result<void, DomainError>::failure(
            sqlError(QStringLiteral("failed to abort private staging"), query.lastError()));
    }
    return Result<void, DomainError>::success();
}

int SqlitePrivatePsycheRepository::innerThoughtCount(
    const QString& profileId) const {
    if (!isOpen()) return 0;
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM inner_thought WHERE profile_id=?"));
    query.addBindValue(profileId);
    return query.exec() && query.next() ? query.value(0).toInt() : 0;
}

int SqlitePrivatePsycheRepository::diaryCount(const QString& profileId) const {
    if (!isOpen()) return 0;
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM diary_entry WHERE profile_id=?"));
    query.addBindValue(profileId);
    return query.exec() && query.next() ? query.value(0).toInt() : 0;
}

int SqlitePrivatePsycheRepository::preparedDiaryCount(
    const QString& sessionId) const {
    if (!isOpen()) return 0;
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM sleep_staged_change "
        "WHERE session_id=? AND target_type='diary_entry' AND status='Prepared'"));
    query.addBindValue(sessionId);
    return query.exec() && query.next() ? query.value(0).toInt() : 0;
}

QByteArray SqlitePrivatePsycheRepository::stagedDiaryCiphertext(
    const QString& sessionId) const {
    if (!isOpen()) return {};
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT ciphertext FROM sleep_staged_change "
        "WHERE session_id=? AND target_type='diary_entry' AND status='Prepared' LIMIT 1"));
    query.addBindValue(sessionId);
    return query.exec() && query.next() ? query.value(0).toByteArray() : QByteArray{};
}
