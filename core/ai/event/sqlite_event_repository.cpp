#include "sqlite_event_repository.h"

#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QUuid>

namespace {

QString newConnectionName(const QString& prefix) {
    return prefix + QLatin1Char('_')
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

DomainError sqliteError(const QString& message, const QSqlError& error = {}) {
    return domainError(QStringLiteral("EVENT_OUTBOX_UNAVAILABLE"),
                       error.isValid() ? message + QStringLiteral(": ") + error.text()
                                       : message);
}

QString compactJson(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

Result<EventRecord, DomainError> recordFromQuery(const QSqlQuery& query) {
    EventRecord record;
    record.sequence = query.value(QStringLiteral("sequence")).toLongLong();
    record.eventId = query.value(QStringLiteral("event_id")).toString();
    record.schemaVersion = query.value(QStringLiteral("schema_version")).toInt();
    record.profileId = query.value(QStringLiteral("profile_id")).toString();
    record.type = query.value(QStringLiteral("type")).toString();
    record.source = query.value(QStringLiteral("source")).toString();
    record.sessionId = query.value(QStringLiteral("session_id")).toString();
    const std::optional<EventPrivacy> privacy =
        eventPrivacyFromString(query.value(QStringLiteral("privacy")).toString());
    if (!privacy.has_value()) {
        return Result<EventRecord, DomainError>::failure(
            sqliteError(QStringLiteral("stored event privacy is invalid")));
    }
    record.privacy = *privacy;

    QJsonParseError payloadError;
    const QJsonDocument payload = QJsonDocument::fromJson(
        query.value(QStringLiteral("payload_json")).toByteArray(), &payloadError);
    if (payloadError.error != QJsonParseError::NoError || !payload.isObject()) {
        return Result<EventRecord, DomainError>::failure(
            sqliteError(QStringLiteral("stored event payload is invalid")));
    }
    record.payload = payload.object();

    const QString referenceJson = query.value(QStringLiteral("private_ref_json")).toString();
    if (!referenceJson.isEmpty()) {
        QJsonParseError referenceError;
        const QJsonDocument reference = QJsonDocument::fromJson(
            referenceJson.toUtf8(), &referenceError);
        if (referenceError.error != QJsonParseError::NoError || !reference.isObject()) {
            return Result<EventRecord, DomainError>::failure(
                sqliteError(QStringLiteral("stored private event reference is invalid")));
        }
        auto parsed = privateEventReferenceFromJson(reference.object());
        if (!parsed.isOk()) {
            return Result<EventRecord, DomainError>::failure(parsed.error());
        }
        record.privateReference = parsed.takeValue();
    }
    record.occurredAt = QDateTime::fromString(
        query.value(QStringLiteral("occurred_at")).toString(), Qt::ISODateWithMs).toUTC();
    record.createdAt = QDateTime::fromString(
        query.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs).toUTC();
    return Result<EventRecord, DomainError>::success(std::move(record));
}

const QStringList kSchemaStatements = {
    QStringLiteral("CREATE TABLE IF NOT EXISTS event_log ("
                   "sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
                   "event_id TEXT NOT NULL UNIQUE, schema_version INTEGER NOT NULL,"
                   "profile_id TEXT NOT NULL, type TEXT NOT NULL, source TEXT NOT NULL,"
                   "session_id TEXT, privacy TEXT NOT NULL DEFAULT 'normal',"
                   "payload_json TEXT NOT NULL, private_ref_json TEXT,"
                   "occurred_at TEXT NOT NULL, created_at TEXT NOT NULL)"),
    QStringLiteral("CREATE INDEX IF NOT EXISTS idx_event_log_type_time "
                   "ON event_log(type, occurred_at)"),
    QStringLiteral("CREATE INDEX IF NOT EXISTS idx_event_log_session_sequence "
                   "ON event_log(session_id, sequence)"),
    QStringLiteral("CREATE TABLE IF NOT EXISTS event_outbox ("
                   "outbox_id TEXT PRIMARY KEY, event_id TEXT NOT NULL UNIQUE,"
                   "payload_json TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'Pending',"
                   "attempt_count INTEGER NOT NULL DEFAULT 0, next_attempt_at TEXT,"
                   "created_at TEXT NOT NULL, delivered_at TEXT)"),
    QStringLiteral("CREATE INDEX IF NOT EXISTS idx_event_outbox_dispatch "
                   "ON event_outbox(status, next_attempt_at, created_at)"),
    QStringLiteral("CREATE TABLE IF NOT EXISTS consumer_checkpoint ("
                   "consumer_id TEXT PRIMARY KEY, last_sequence INTEGER NOT NULL DEFAULT 0,"
                   "updated_at TEXT NOT NULL)"),
    QStringLiteral("CREATE TABLE IF NOT EXISTS personality_state ("
                   "state_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL, version INTEGER NOT NULL,"
                   "baseline_json TEXT NOT NULL, tendency_json TEXT NOT NULL,"
                   "evidence_cutoff_sequence INTEGER NOT NULL, effective_at TEXT NOT NULL,"
                   "created_at TEXT NOT NULL, UNIQUE(profile_id, version))"),
    QStringLiteral("CREATE TABLE IF NOT EXISTS relationship_state ("
                   "state_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL, subject_id TEXT NOT NULL,"
                   "version INTEGER NOT NULL, state_json TEXT NOT NULL,"
                   "evidence_cutoff_sequence INTEGER NOT NULL, effective_at TEXT NOT NULL,"
                   "created_at TEXT NOT NULL, UNIQUE(profile_id, subject_id, version))"),
    QStringLiteral("CREATE TABLE IF NOT EXISTS self_model_version ("
                   "version_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL, parent_version_id TEXT,"
                   "narrative_json TEXT NOT NULL, evidence_json TEXT NOT NULL,"
                   "effective_at TEXT NOT NULL, created_at TEXT NOT NULL,"
                   "FOREIGN KEY(parent_version_id) REFERENCES self_model_version(version_id))"),
    QStringLiteral("CREATE TABLE IF NOT EXISTS trait_evidence ("
                   "evidence_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL, trait TEXT NOT NULL,"
                   "source_event_id TEXT, direction REAL NOT NULL, weight REAL NOT NULL,"
                   "confidence REAL NOT NULL, context_key TEXT, status TEXT NOT NULL,"
                   "created_at TEXT NOT NULL)"),
    QStringLiteral("CREATE INDEX IF NOT EXISTS idx_trait_evidence_window "
                   "ON trait_evidence(profile_id, trait, status, created_at)"),
    QStringLiteral("CREATE TABLE IF NOT EXISTS sleep_session ("
                   "session_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL,"
                   "source_cutoff_sequence INTEGER NOT NULL, state TEXT NOT NULL,"
                   "decision TEXT NOT NULL DEFAULT 'Pending', participants_json TEXT NOT NULL,"
                   "finalized_participants_json TEXT NOT NULL DEFAULT '[]',"
                   "revision INTEGER NOT NULL DEFAULT 1, started_at TEXT NOT NULL,"
                   "updated_at TEXT NOT NULL, decision_at TEXT, completed_at TEXT)"),
    QStringLiteral("CREATE INDEX IF NOT EXISTS idx_sleep_session_recovery "
                   "ON sleep_session(decision, state, updated_at)"),
    QStringLiteral("CREATE TABLE IF NOT EXISTS sleep_staged_change ("
                   "session_id TEXT NOT NULL, change_id TEXT NOT NULL, target_type TEXT NOT NULL,"
                   "operation TEXT NOT NULL, payload_json TEXT NOT NULL, payload_hash TEXT NOT NULL,"
                   "status TEXT NOT NULL DEFAULT 'Prepared', created_at TEXT NOT NULL,"
                   "finalized_at TEXT, PRIMARY KEY(session_id, change_id),"
                   "FOREIGN KEY(session_id) REFERENCES sleep_session(session_id))"),
    QStringLiteral("CREATE INDEX IF NOT EXISTS idx_runtime_sleep_staged_status "
                   "ON sleep_staged_change(session_id, status)")
};

} // namespace

SqliteEventRepository::SqliteEventRepository()
    : m_connectionName(newConnectionName(QStringLiteral("event_repository"))) {}

SqliteEventRepository::~SqliteEventRepository() {
    close();
}

Result<void, DomainError> SqliteEventRepository::open(const QString& databasePath) {
    if (databasePath.trimmed().isEmpty()) {
        return Result<void, DomainError>::failure(
            sqliteError(QStringLiteral("runtime database path is empty")));
    }
    close();
    m_connectionName = newConnectionName(QStringLiteral("event_repository"));
    m_databasePath = databasePath;
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    database.setDatabaseName(databasePath);
    if (!database.open()) {
        const DomainError error = sqliteError(
            QStringLiteral("failed to open runtime database"), database.lastError());
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_databasePath.clear();
        return Result<void, DomainError>::failure(error);
    }
    Result<void, DomainError> migration = migrateSchema(database);
    if (!migration.isOk()) {
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_databasePath.clear();
        return migration;
    }
    return Result<void, DomainError>::success();
}

void SqliteEventRepository::close() {
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

bool SqliteEventRepository::isOpen() const {
    if (m_connectionName.isEmpty() || !QSqlDatabase::contains(m_connectionName)) return false;
    return QSqlDatabase::database(m_connectionName, false).isOpen();
}

Result<void, DomainError> SqliteEventRepository::migrateSchema(QSqlDatabase& database) {
    QSqlQuery pragma(database);
    if (!pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"))
        || !pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"))
        || !pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"))) {
        return Result<void, DomainError>::failure(
            sqliteError(QStringLiteral("failed to configure runtime database"), pragma.lastError()));
    }
    if (!pragma.exec(QStringLiteral("PRAGMA user_version")) || !pragma.next()) {
        return Result<void, DomainError>::failure(
            sqliteError(QStringLiteral("failed to read runtime schema version"), pragma.lastError()));
    }
    const int version = pragma.value(0).toInt();
    if (version > 1) {
        return Result<void, DomainError>::failure(
            sqliteError(QStringLiteral("runtime schema version is newer than supported")));
    }
    if (!database.transaction()) {
        return Result<void, DomainError>::failure(
            sqliteError(QStringLiteral("failed to begin runtime schema migration"),
                        database.lastError()));
    }
    QSqlQuery query(database);
    for (const QString& statement : kSchemaStatements) {
        if (!query.exec(statement)) {
            database.rollback();
            return Result<void, DomainError>::failure(
                sqliteError(QStringLiteral("failed to migrate runtime schema"), query.lastError()));
        }
    }
    if (!query.exec(QStringLiteral("PRAGMA user_version=1")) || !database.commit()) {
        database.rollback();
        return Result<void, DomainError>::failure(
            sqliteError(QStringLiteral("failed to commit runtime schema migration"),
                        query.lastError().isValid() ? query.lastError() : database.lastError()));
    }
    return Result<void, DomainError>::success();
}

Result<EventRecord, DomainError> SqliteEventRepository::insertEvent(
    QSqlDatabase& database, const EventDraft& draft, bool idempotent) {
    QSqlQuery query(database);
    query.prepare(idempotent
        ? QStringLiteral("INSERT OR IGNORE INTO event_log("
                         "event_id,schema_version,profile_id,type,source,session_id,privacy,"
                         "payload_json,private_ref_json,occurred_at,created_at) "
                         "VALUES(?,?,?,?,?,?,?,?,?,?,?)")
        : QStringLiteral("INSERT INTO event_log("
                         "event_id,schema_version,profile_id,type,source,session_id,privacy,"
                         "payload_json,private_ref_json,occurred_at,created_at) "
                         "VALUES(?,?,?,?,?,?,?,?,?,?,?)"));
    const QDateTime createdAt = QDateTime::currentDateTimeUtc();
    query.addBindValue(draft.eventId);
    query.addBindValue(draft.schemaVersion);
    query.addBindValue(draft.profileId);
    query.addBindValue(draft.type);
    query.addBindValue(draft.source);
    query.addBindValue(draft.sessionId.isEmpty() ? QVariant() : QVariant(draft.sessionId));
    query.addBindValue(eventPrivacyToString(draft.privacy));
    query.addBindValue(compactJson(draft.payload));
    query.addBindValue(draft.privateReference.has_value()
        ? QVariant(compactJson(privateEventReferenceToJson(*draft.privateReference)))
        : QVariant());
    query.addBindValue(draft.occurredAt.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(createdAt.toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        return Result<EventRecord, DomainError>::failure(
            sqliteError(QStringLiteral("failed to insert event"), query.lastError()));
    }

    QSqlQuery select(database);
    select.prepare(QStringLiteral("SELECT * FROM event_log WHERE event_id=?"));
    select.addBindValue(draft.eventId);
    if (!select.exec() || !select.next()) {
        return Result<EventRecord, DomainError>::failure(
            sqliteError(QStringLiteral("failed to read inserted event"), select.lastError()));
    }
    return recordFromQuery(select);
}

Result<EventRecord, DomainError> SqliteEventRepository::append(const EventDraft& draft) {
    if (!isOpen()) {
        return Result<EventRecord, DomainError>::failure(
            sqliteError(QStringLiteral("runtime event repository is closed")));
    }
    QSqlDatabase database = QSqlDatabase::database(m_connectionName);
    if (!database.transaction()) {
        return Result<EventRecord, DomainError>::failure(
            sqliteError(QStringLiteral("failed to begin event append transaction"),
                        database.lastError()));
    }
    auto inserted = insertEvent(database, draft, false);
    if (!inserted.isOk() || !database.commit()) {
        const DomainError error = inserted.isOk()
            ? sqliteError(QStringLiteral("failed to commit event append"), database.lastError())
            : inserted.error();
        database.rollback();
        return Result<EventRecord, DomainError>::failure(error);
    }
    return inserted;
}

Result<QList<EventRecord>, DomainError> SqliteEventRepository::readAfter(
    qint64 sequence, const EventFilter& filter, int limit) const {
    if (!isOpen()) {
        return Result<QList<EventRecord>, DomainError>::failure(
            sqliteError(QStringLiteral("runtime event repository is closed")));
    }
    QSqlDatabase database = QSqlDatabase::database(m_connectionName);
    QString sql = QStringLiteral("SELECT * FROM event_log WHERE sequence > ?");
    if (!filter.sessionId.isEmpty()) sql += QStringLiteral(" AND session_id = ?");
    if (!filter.types.isEmpty()) {
        QStringList placeholders;
        placeholders.fill(QStringLiteral("?"), filter.types.size());
        sql += QStringLiteral(" AND type IN (") + placeholders.join(QLatin1Char(','))
            + QLatin1Char(')');
    }
    sql += QStringLiteral(" ORDER BY sequence ASC");
    QSqlQuery query(database);
    query.prepare(sql);
    query.addBindValue(sequence);
    if (!filter.sessionId.isEmpty()) query.addBindValue(filter.sessionId);
    for (const QString& type : filter.types) query.addBindValue(type);
    if (!query.exec()) {
        return Result<QList<EventRecord>, DomainError>::failure(
            sqliteError(QStringLiteral("failed to read events"), query.lastError()));
    }

    QList<EventRecord> records;
    while (query.next() && records.size() < limit) {
        auto parsed = recordFromQuery(query);
        if (!parsed.isOk()) {
            return Result<QList<EventRecord>, DomainError>::failure(parsed.error());
        }
        EventRecord record = parsed.takeValue();
        if (record.privacy == EventPrivacy::Sensitive
            && !filter.authorization.allowsSensitivePayload()) {
            continue;
        }
        if (record.privacy == EventPrivacy::Private
            && (!record.privateReference.has_value()
                || !filter.authorization.allowsPrivateReference(
                    record.privateReference->recordType))) {
            continue;
        }
        records.append(std::move(record));
    }
    return Result<QList<EventRecord>, DomainError>::success(std::move(records));
}

Result<qint64, DomainError> SqliteEventRepository::currentCheckpoint(
    const QString& consumerId) const {
    if (!isOpen() || consumerId.trimmed().isEmpty()) {
        return Result<qint64, DomainError>::failure(
            sqliteError(QStringLiteral("checkpoint request is invalid")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT last_sequence FROM consumer_checkpoint WHERE consumer_id=?"));
    query.addBindValue(consumerId);
    if (!query.exec()) {
        return Result<qint64, DomainError>::failure(
            sqliteError(QStringLiteral("failed to read checkpoint"), query.lastError()));
    }
    return Result<qint64, DomainError>::success(query.next() ? query.value(0).toLongLong() : 0);
}

Result<void, DomainError> SqliteEventRepository::commitCheckpoint(
    const QString& consumerId, qint64 expectedSequence, qint64 nextSequence) {
    if (!isOpen() || consumerId.trimmed().isEmpty()
        || expectedSequence < 0 || nextSequence < expectedSequence) {
        return Result<void, DomainError>::failure(
            sqliteError(QStringLiteral("checkpoint commit request is invalid")));
    }
    QSqlDatabase database = QSqlDatabase::database(m_connectionName);
    if (!database.transaction()) {
        return Result<void, DomainError>::failure(
            sqliteError(QStringLiteral("failed to begin checkpoint transaction"),
                        database.lastError()));
    }
    QSqlQuery current(database);
    current.prepare(QStringLiteral(
        "SELECT last_sequence FROM consumer_checkpoint WHERE consumer_id=?"));
    current.addBindValue(consumerId);
    if (!current.exec()) {
        database.rollback();
        return Result<void, DomainError>::failure(
            sqliteError(QStringLiteral("failed to read checkpoint"), current.lastError()));
    }
    const bool exists = current.next();
    const qint64 actual = exists ? current.value(0).toLongLong() : 0;
    if (actual != expectedSequence) {
        database.rollback();
        return Result<void, DomainError>::failure(
            domainError(QStringLiteral("STATE_VERSION_CONFLICT"),
                        QStringLiteral("consumer checkpoint changed")));
    }

    QSqlQuery write(database);
    if (exists) {
        write.prepare(QStringLiteral(
            "UPDATE consumer_checkpoint SET last_sequence=?,updated_at=? "
            "WHERE consumer_id=? AND last_sequence=?"));
        write.addBindValue(nextSequence);
        write.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        write.addBindValue(consumerId);
        write.addBindValue(expectedSequence);
    } else {
        write.prepare(QStringLiteral(
            "INSERT INTO consumer_checkpoint(consumer_id,last_sequence,updated_at) "
            "VALUES(?,?,?)"));
        write.addBindValue(consumerId);
        write.addBindValue(nextSequence);
        write.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    }
    if (!write.exec() || (exists && write.numRowsAffected() != 1) || !database.commit()) {
        database.rollback();
        return Result<void, DomainError>::failure(
            sqliteError(QStringLiteral("failed to commit checkpoint"),
                        write.lastError().isValid() ? write.lastError() : database.lastError()));
    }
    return Result<void, DomainError>::success();
}
