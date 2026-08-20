#include "sqlite_emotion_state_repository.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <utility>

SQLiteEmotionStateRepository::SQLiteEmotionStateRepository(QString databasePath)
    : m_databasePath(QDir::cleanPath(std::move(databasePath))),
      m_connectionName(QStringLiteral("emotion_")
                           + QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

SQLiteEmotionStateRepository::~SQLiteEmotionStateRepository() {
    close();
}

std::optional<PersistedEmotionState> SQLiteEmotionStateRepository::load() {
    if (!ensureOpen()) {
        return std::nullopt;
    }

    QSqlDatabase database = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT schema_version, mood_valence, mood_arousal, updated_at_utc, personality_revision "
        "FROM emotion_state WHERE id = 1"));
    if (!query.exec()) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    if (!query.next()) {
        m_lastError.clear();
        return std::nullopt;
    }

    PersistedEmotionState state;
    state.schemaVersion = query.value(0).toInt();
    state.moodValence = query.value(1).toDouble();
    state.moodArousal = query.value(2).toDouble();
    state.updatedAtUtc = QDateTime::fromString(query.value(3).toString(), Qt::ISODateWithMs);
    if (!state.updatedAtUtc.isValid()) {
        state.updatedAtUtc = QDateTime::fromString(query.value(3).toString(), Qt::ISODate);
    }
    state.personalityRevision = query.value(4).toInt();
    m_lastError.clear();
    return state;
}

bool SQLiteEmotionStateRepository::save(const PersistedEmotionState& state) {
    if (!ensureOpen()) {
        return false;
    }

    QSqlDatabase database = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO emotion_state "
        "(id, schema_version, mood_valence, mood_arousal, updated_at_utc, personality_revision) "
        "VALUES (1, :schema_version, :mood_valence, :mood_arousal, :updated_at_utc, :personality_revision) "
        "ON CONFLICT(id) DO UPDATE SET "
        "schema_version = excluded.schema_version, "
        "mood_valence = excluded.mood_valence, "
        "mood_arousal = excluded.mood_arousal, "
        "updated_at_utc = excluded.updated_at_utc, "
        "personality_revision = excluded.personality_revision"));
    query.bindValue(QStringLiteral(":schema_version"), state.schemaVersion);
    query.bindValue(QStringLiteral(":mood_valence"), state.moodValence);
    query.bindValue(QStringLiteral(":mood_arousal"), state.moodArousal);
    query.bindValue(QStringLiteral(":updated_at_utc"),
                    state.updatedAtUtc.toUTC().toString(Qt::ISODateWithMs));
    query.bindValue(QStringLiteral(":personality_revision"), state.personalityRevision);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    m_lastError.clear();
    return true;
}

bool SQLiteEmotionStateRepository::isOpen() const {
    if (!QSqlDatabase::contains(m_connectionName)) {
        return false;
    }
    return QSqlDatabase::database(m_connectionName, false).isOpen();
}

void SQLiteEmotionStateRepository::close() {
    if (!QSqlDatabase::contains(m_connectionName)) {
        return;
    }
    {
        QSqlDatabase database = QSqlDatabase::database(m_connectionName, false);
        database.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool SQLiteEmotionStateRepository::ensureOpen() {
    if (isOpen()) {
        return true;
    }
    if (m_databasePath.trimmed().isEmpty()) {
        setError(QStringLiteral("emotion database path is empty"));
        return false;
    }

    const QFileInfo info(m_databasePath);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().path())) {
        setError(QStringLiteral("failed to create emotion database directory: %1")
                     .arg(info.dir().path()));
        return false;
    }

    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    database.setDatabaseName(m_databasePath);
    if (!database.open()) {
        setError(database.lastError().text());
        database = {};
        QSqlDatabase::removeDatabase(m_connectionName);
        return false;
    }

    QSqlQuery pragma(database);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    return initializeSchema();
}

bool SQLiteEmotionStateRepository::initializeSchema() {
    QSqlDatabase database = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS emotion_state ("
            "id INTEGER PRIMARY KEY CHECK(id = 1), "
            "schema_version INTEGER NOT NULL, "
            "mood_valence REAL NOT NULL, "
            "mood_arousal REAL NOT NULL, "
            "updated_at_utc TEXT NOT NULL, "
            "personality_revision INTEGER NOT NULL)"))) {
        setError(query.lastError().text());
        close();
        return false;
    }
    m_lastError.clear();
    return true;
}

void SQLiteEmotionStateRepository::setError(const QString& error) {
    m_lastError = error.trimmed().left(1024);
}
