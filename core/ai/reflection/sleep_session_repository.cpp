#include "sleep_session_repository.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <utility>

namespace {

QString connectionName() {
    return QStringLiteral("sleep_session_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
}

DomainError repositoryError(const QString& message,
                            const QSqlError& error = {}) {
    return domainError(
        QStringLiteral("EVENT_OUTBOX_UNAVAILABLE"),
        error.isValid() ? message + QStringLiteral(": ") + error.text() : message);
}

QString stringListJson(const QStringList& values) {
    return QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(values))
            .toJson(QJsonDocument::Compact));
}

QStringList parseStringList(const QString& json) {
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isArray()) return {};
    QStringList values;
    for (const QJsonValue& value : document.array()) {
        const QString text = value.toString();
        if (!text.isEmpty() && !values.contains(text)) values.append(text);
    }
    return values;
}

SleepSessionRecord recordFromQuery(const QSqlQuery& query) {
    SleepSessionRecord record;
    record.sessionId = query.value(QStringLiteral("session_id")).toString();
    record.profileId = query.value(QStringLiteral("profile_id")).toString();
    record.sourceCutoffSequence =
        query.value(QStringLiteral("source_cutoff_sequence")).toLongLong();
    record.state = sleepSessionStateFromString(
        query.value(QStringLiteral("state")).toString())
                       .value_or(SleepSessionState::Snapshotting);
    record.decision = sleepDecisionFromString(
        query.value(QStringLiteral("decision")).toString())
                          .value_or(SleepDecision::Pending);
    record.participants = parseStringList(
        query.value(QStringLiteral("participants_json")).toString());
    record.finalizedParticipants = parseStringList(
        query.value(QStringLiteral("finalized_participants_json")).toString());
    record.revision = query.value(QStringLiteral("revision")).toInt();
    record.startedAt = QDateTime::fromString(
        query.value(QStringLiteral("started_at")).toString(), Qt::ISODateWithMs);
    record.updatedAt = QDateTime::fromString(
        query.value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs);
    record.decisionAt = QDateTime::fromString(
        query.value(QStringLiteral("decision_at")).toString(), Qt::ISODateWithMs);
    record.completedAt = QDateTime::fromString(
        query.value(QStringLiteral("completed_at")).toString(), Qt::ISODateWithMs);
    return record;
}

} // namespace

SleepSessionRepository::SleepSessionRepository()
    : m_connectionName(connectionName()) {}

SleepSessionRepository::~SleepSessionRepository() {
    close();
}

Result<void, DomainError> SleepSessionRepository::open(
    const QString& databasePath) {
    if (databasePath.trimmed().isEmpty()) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("sleep session database path is empty")));
    }
    close();
    const QFileInfo info(databasePath);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().path())) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to create sleep database directory")));
    }
    m_connectionName = connectionName();
    QSqlDatabase database = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), m_connectionName);
    database.setDatabaseName(databasePath);
    if (!database.open()) {
        const DomainError error = repositoryError(
            QStringLiteral("failed to open sleep session database"),
            database.lastError());
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

void SleepSessionRepository::close() {
    if (!m_connectionName.isEmpty() && QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase database = QSqlDatabase::database(m_connectionName, false);
            if (database.isValid()) database.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
    m_databasePath.clear();
}

bool SleepSessionRepository::isOpen() const {
    return QSqlDatabase::contains(m_connectionName)
        && QSqlDatabase::database(m_connectionName, false).isOpen();
}

Result<void, DomainError> SleepSessionRepository::migrateSchema(
    QSqlDatabase& database) {
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA foreign_keys=ON"))
        || !query.exec(QStringLiteral("PRAGMA journal_mode=WAL"))
        || !query.exec(QStringLiteral("PRAGMA busy_timeout=5000"))) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to configure sleep database"),
                            query.lastError()));
    }
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to read runtime schema version"),
                            query.lastError()));
    }
    const int version = query.value(0).toInt();
    if (version < 0 || version > 1) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("runtime schema version is newer than supported")));
    }
    if (!database.transaction()) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to begin sleep schema migration"),
                            database.lastError()));
    }
    const QStringList statements{
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS sleep_session("
            "session_id TEXT PRIMARY KEY,profile_id TEXT NOT NULL,"
            "source_cutoff_sequence INTEGER NOT NULL,state TEXT NOT NULL,"
            "decision TEXT NOT NULL DEFAULT 'Pending',participants_json TEXT NOT NULL,"
            "finalized_participants_json TEXT NOT NULL DEFAULT '[]',"
            "revision INTEGER NOT NULL DEFAULT 1,started_at TEXT NOT NULL,"
            "updated_at TEXT NOT NULL,decision_at TEXT,completed_at TEXT)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_sleep_session_recovery "
            "ON sleep_session(decision,state,updated_at)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS sleep_staged_change("
            "session_id TEXT NOT NULL,change_id TEXT NOT NULL,target_type TEXT NOT NULL,"
            "operation TEXT NOT NULL,payload_json TEXT NOT NULL,payload_hash TEXT NOT NULL,"
            "status TEXT NOT NULL DEFAULT 'Prepared',created_at TEXT NOT NULL,"
            "finalized_at TEXT,PRIMARY KEY(session_id,change_id),"
            "FOREIGN KEY(session_id) REFERENCES sleep_session(session_id))"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_runtime_sleep_staged_status "
            "ON sleep_staged_change(session_id,status)")
    };
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            database.rollback();
            return Result<void, DomainError>::failure(
                repositoryError(QStringLiteral("failed to migrate sleep schema"),
                                query.lastError()));
        }
    }
    if (version == 0 && !query.exec(QStringLiteral("PRAGMA user_version=1"))) {
        database.rollback();
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to set runtime schema version"),
                            query.lastError()));
    }
    if (!database.commit()) {
        database.rollback();
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to commit sleep schema migration"),
                            database.lastError()));
    }
    return Result<void, DomainError>::success();
}

Result<void, DomainError> SleepSessionRepository::createPending(
    const SleepSessionRecord& input) {
    if (!isOpen() || input.sessionId.trimmed().isEmpty()
        || !isCanonicalEventUuid(input.profileId)) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("sleep session is invalid")));
    }
    SleepSessionRecord session = input;
    session.decision = SleepDecision::Pending;
    if (!session.startedAt.isValid()) session.startedAt = QDateTime::currentDateTimeUtc();
    session.updatedAt = session.startedAt;
    if (session.participants.isEmpty()) {
        session.participants = {QStringLiteral("memory"),
                                QStringLiteral("private_psyche")};
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "INSERT INTO sleep_session(session_id,profile_id,source_cutoff_sequence,state,"
        "decision,participants_json,finalized_participants_json,revision,started_at,updated_at)"
        " VALUES(?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(session.sessionId);
    query.addBindValue(session.profileId);
    query.addBindValue(session.sourceCutoffSequence);
    query.addBindValue(sleepSessionStateToString(session.state));
    query.addBindValue(sleepDecisionToString(session.decision));
    query.addBindValue(stringListJson(session.participants));
    query.addBindValue(stringListJson(session.finalizedParticipants));
    query.addBindValue(qMax(1, session.revision));
    query.addBindValue(session.startedAt.toString(Qt::ISODateWithMs));
    query.addBindValue(session.updatedAt.toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to create sleep session"),
                            query.lastError()));
    }
    return Result<void, DomainError>::success();
}

Result<std::optional<SleepSessionRecord>, DomainError>
SleepSessionRepository::find(const QString& sessionId) const {
    if (!isOpen()) {
        return Result<std::optional<SleepSessionRecord>, DomainError>::failure(
            repositoryError(QStringLiteral("sleep repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral("SELECT * FROM sleep_session WHERE session_id=?"));
    query.addBindValue(sessionId);
    if (!query.exec()) {
        return Result<std::optional<SleepSessionRecord>, DomainError>::failure(
            repositoryError(QStringLiteral("failed to read sleep session"),
                            query.lastError()));
    }
    if (!query.next()) {
        return Result<std::optional<SleepSessionRecord>, DomainError>::success(std::nullopt);
    }
    return Result<std::optional<SleepSessionRecord>, DomainError>::success(
        recordFromQuery(query));
}

Result<QList<SleepSessionRecord>, DomainError> SleepSessionRepository::incomplete(
    const QString& profileId) const {
    if (!isOpen()) {
        return Result<QList<SleepSessionRecord>, DomainError>::failure(
            repositoryError(QStringLiteral("sleep repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT * FROM sleep_session WHERE profile_id=? "
        "AND state NOT IN ('Completed','RolledBack') ORDER BY started_at,session_id"));
    query.addBindValue(profileId);
    if (!query.exec()) {
        return Result<QList<SleepSessionRecord>, DomainError>::failure(
            repositoryError(QStringLiteral("failed to scan sleep recovery"),
                            query.lastError()));
    }
    QList<SleepSessionRecord> sessions;
    while (query.next()) sessions.append(recordFromQuery(query));
    return Result<QList<SleepSessionRecord>, DomainError>::success(std::move(sessions));
}

Result<qint64, DomainError> SleepSessionRepository::latestEventSequence(
    const QString& profileId) const {
    if (!isOpen() || !isCanonicalEventUuid(profileId)) {
        return Result<qint64, DomainError>::failure(
            repositoryError(QStringLiteral("sleep event cutoff query is invalid")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT COALESCE(MAX(sequence),0) FROM event_log WHERE profile_id=?"));
    query.addBindValue(profileId);
    if (!query.exec() || !query.next()) {
        return Result<qint64, DomainError>::failure(
            repositoryError(QStringLiteral("failed to read sleep event cutoff"),
                            query.lastError()));
    }
    return Result<qint64, DomainError>::success(qMax<qint64>(0, query.value(0).toLongLong()));
}

Result<void, DomainError> SleepSessionRepository::updateState(
    const QString& sessionId,
    SleepSessionState state) {
    if (!isOpen()) return Result<void, DomainError>::failure(
        repositoryError(QStringLiteral("sleep repository is closed")));
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "UPDATE sleep_session SET state=?,updated_at=?,revision=revision+1 "
        "WHERE session_id=?"));
    query.addBindValue(sleepSessionStateToString(state));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(sessionId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to update sleep session state"),
                            query.lastError()));
    }
    return Result<void, DomainError>::success();
}

Result<void, DomainError> SleepSessionRepository::decideCommit(
    const QString& sessionId) {
    const auto current = find(sessionId);
    if (!current.isOk() || !current.value().has_value()) {
        return Result<void, DomainError>::failure(
            current.isOk() ? repositoryError(QStringLiteral("sleep session was not found"))
                           : current.error());
    }
    if (current.value()->decision == SleepDecision::Abort) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("aborted sleep session cannot commit")));
    }
    if (current.value()->decision == SleepDecision::Commit) {
        return Result<void, DomainError>::success();
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "UPDATE sleep_session SET decision='Commit',state='Committing',decision_at=?,"
        "updated_at=?,revision=revision+1 WHERE session_id=? AND decision='Pending'"));
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    query.addBindValue(now);
    query.addBindValue(now);
    query.addBindValue(sessionId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to persist sleep Commit decision"),
                            query.lastError()));
    }
    return Result<void, DomainError>::success();
}

Result<void, DomainError> SleepSessionRepository::decideAbort(
    const QString& sessionId) {
    const auto current = find(sessionId);
    if (!current.isOk() || !current.value().has_value()) {
        return Result<void, DomainError>::failure(
            current.isOk() ? repositoryError(QStringLiteral("sleep session was not found"))
                           : current.error());
    }
    if (current.value()->decision == SleepDecision::Commit) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("committed sleep session cannot abort")));
    }
    if (current.value()->decision == SleepDecision::Abort) {
        return Result<void, DomainError>::success();
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "UPDATE sleep_session SET decision='Abort',state='Cancelling',decision_at=?,"
        "updated_at=?,revision=revision+1 WHERE session_id=? AND decision='Pending'"));
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    query.addBindValue(now);
    query.addBindValue(now);
    query.addBindValue(sessionId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to persist sleep Abort decision"),
                            query.lastError()));
    }
    return Result<void, DomainError>::success();
}

Result<void, DomainError> SleepSessionRepository::markParticipantFinalized(
    const QString& sessionId,
    const QString& participant) {
    const auto current = find(sessionId);
    if (!current.isOk() || !current.value().has_value()) {
        return Result<void, DomainError>::failure(
            current.isOk() ? repositoryError(QStringLiteral("sleep session was not found"))
                           : current.error());
    }
    if (current.value()->decision != SleepDecision::Commit) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("participant cannot finalize before Commit")));
    }
    QStringList finalized = current.value()->finalizedParticipants;
    if (!finalized.contains(participant)) finalized.append(participant);
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "UPDATE sleep_session SET finalized_participants_json=?,updated_at=?,"
        "revision=revision+1 WHERE session_id=? AND decision='Commit'"));
    query.addBindValue(stringListJson(finalized));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(sessionId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to mark sleep participant finalized"),
                            query.lastError()));
    }
    return Result<void, DomainError>::success();
}

Result<void, DomainError> SleepSessionRepository::markCompleted(
    const QString& sessionId) {
    if (!isOpen()) return Result<void, DomainError>::failure(
        repositoryError(QStringLiteral("sleep repository is closed")));
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "UPDATE sleep_session SET state='Completed',completed_at=?,updated_at=?,"
        "revision=revision+1 WHERE session_id=? AND decision='Commit'"));
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    query.addBindValue(now);
    query.addBindValue(now);
    query.addBindValue(sessionId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to complete sleep session"),
                            query.lastError()));
    }
    return Result<void, DomainError>::success();
}

Result<void, DomainError> SleepSessionRepository::markRolledBack(
    const QString& sessionId) {
    if (!isOpen()) return Result<void, DomainError>::failure(
        repositoryError(QStringLiteral("sleep repository is closed")));
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "UPDATE sleep_session SET state='RolledBack',completed_at=?,updated_at=?,"
        "revision=revision+1 WHERE session_id=? AND decision='Abort'"));
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    query.addBindValue(now);
    query.addBindValue(now);
    query.addBindValue(sessionId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        return Result<void, DomainError>::failure(
            repositoryError(QStringLiteral("failed to roll back sleep session"),
                            query.lastError()));
    }
    return Result<void, DomainError>::success();
}
