#include "task_store.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

TaskStore::~TaskStore() { close(); }

bool TaskStore::open(const QString& path) {
    close();
    if (path != ":memory:" && !QDir().mkpath(QFileInfo(path).absolutePath())) {
        m_error = "Cannot create task database directory"; return false;
    }
    m_connectionName = "task-runtime-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    db.setDatabaseName(path);
    if (!db.open()) { m_error = db.lastError().text(); return false; }
    QSqlQuery q(db);
    const QStringList statements{
        "PRAGMA busy_timeout=5000", "PRAGMA synchronous=FULL",
        "CREATE TABLE IF NOT EXISTS complex_tasks (id TEXT PRIMARY KEY, terminal INTEGER NOT NULL, updated_at TEXT NOT NULL, record_json TEXT NOT NULL)",
        "CREATE UNIQUE INDEX IF NOT EXISTS complex_tasks_single_unfinished ON complex_tasks(terminal) WHERE terminal=0",
        "CREATE TABLE IF NOT EXISTS complex_task_experience_outbox (task_id TEXT PRIMARY KEY, payload_json TEXT NOT NULL, delivered INTEGER NOT NULL DEFAULT 0)"};
    for (const auto& sql : statements) if (!q.exec(sql)) { m_error = q.lastError().text(); return false; }
    m_error.clear(); return true;
}

void TaskStore::close() {
    if (m_connectionName.isEmpty()) return;
    { auto db = QSqlDatabase::database(m_connectionName, false); db.close(); }
    QSqlDatabase::removeDatabase(m_connectionName); m_connectionName.clear();
}
bool TaskStore::isOpen() const {
    return !m_connectionName.isEmpty() && QSqlDatabase::database(m_connectionName, false).isOpen();
}
bool TaskStore::save(const TaskRecord& task, bool enqueue) {
    if (!isOpen()) { m_error = "Task database is unavailable"; return false; }
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction()) { m_error = db.lastError().text(); return false; }
    QSqlQuery q(db);
    q.prepare("INSERT INTO complex_tasks(id,terminal,updated_at,record_json) VALUES(?,?,?,?) ON CONFLICT(id) DO UPDATE SET terminal=excluded.terminal,updated_at=excluded.updated_at,record_json=excluded.record_json");
    q.addBindValue(task.id); q.addBindValue(task.terminal() ? 1 : 0); q.addBindValue(task.updatedAt);
    q.addBindValue(QString::fromUtf8(QJsonDocument(task.toJson()).toJson(QJsonDocument::Compact)));
    if (!q.exec()) { m_error = q.lastError().text(); db.rollback(); return false; }
    if (enqueue) {
        // Durable factual summary excludes raw tool output and model working context.
        const QJsonObject payload{{"task_id", task.id}, {"profile_id", task.profileId}, {"subject_id", task.subjectId},
            {"origin_kind", originKindToString(task.originKind)},
            {"goal", task.goal}, {"status", task.status}, {"result", task.result}, {"reason", task.reason},
            {"personality_version", task.personalityVersion}, {"evidence", task.evidence},
            {"initial_emotion", task.initialEmotion}, {"current_emotion", task.currentEmotion},
            {"feedback_status", task.userFeedback.isEmpty() ? "unknown" : "provided"},
            {"user_feedback", task.userFeedback}, {"confidence", task.status == "Completed" ? "verified" : "limited"},
            {"requirement_updates", task.requirementUpdates},
            {"created_at", task.updatedAt}};
        q.prepare("INSERT OR IGNORE INTO complex_task_experience_outbox(task_id,payload_json) VALUES(?,?)");
        q.addBindValue(task.id); q.addBindValue(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
        if (!q.exec()) { m_error = q.lastError().text(); db.rollback(); return false; }
    }
    if (!db.commit()) { m_error = db.lastError().text(); db.rollback(); return false; }
    m_error.clear(); return true;
}
std::optional<TaskRecord> TaskStore::unfinished() const {
    if (!isOpen()) return {};
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    if (!q.exec("SELECT record_json FROM complex_tasks WHERE terminal=0 LIMIT 1")) { m_error = q.lastError().text(); return {}; }
    if (!q.next()) return {};
    return TaskRecord::fromJson(QJsonDocument::fromJson(q.value(0).toByteArray()).object());
}
std::optional<TaskRecord> TaskStore::find(const QString& id) const {
    if (!isOpen()) return {};
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare("SELECT record_json FROM complex_tasks WHERE id=?"); q.addBindValue(id);
    if (!q.exec()) { m_error = q.lastError().text(); return {}; }
    if (!q.next()) return {};
    return TaskRecord::fromJson(QJsonDocument::fromJson(q.value(0).toByteArray()).object());
}
QJsonArray TaskStore::pendingExperiences() const {
    QJsonArray result;
    if (!isOpen()) return result;
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    if (!q.exec("SELECT payload_json FROM complex_task_experience_outbox WHERE delivered=0 ORDER BY rowid")) { m_error = q.lastError().text(); return result; }
    while (q.next()) result.append(QJsonDocument::fromJson(q.value(0).toByteArray()).object());
    return result;
}
bool TaskStore::acknowledgeExperience(const QString& id) {
    if (!isOpen()) return false;
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare("UPDATE complex_task_experience_outbox SET delivered=1 WHERE task_id=?"); q.addBindValue(id);
    if (!q.exec()) { m_error = q.lastError().text(); return false; }
    return q.numRowsAffected() == 1;
}
