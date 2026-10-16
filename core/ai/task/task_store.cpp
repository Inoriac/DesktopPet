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
bool TaskStore::open(const QString &path, const QString &profile) {
    close();
    m_activity = std::make_unique<ActivityStore>(profile, path);
    auto r = m_activity->initialize();
    if (!r) {
        m_error = r.error().message;
        m_activity.reset();
        return false;
    }
    m_error.clear();
    return true;
}
QList<TaskRecord> TaskStore::unfinishedTasks() const {
    QList<TaskRecord> tasks;
    if (!m_activity) {
        auto t = unfinished();
        if (t)
            tasks.append(*t);
        return tasks;
    }
    auto rows =
        m_activity->list({ActivityState::Ready, ActivityState::Running,
                          ActivityState::WaitingExternal,
                          ActivityState::WaitingUser, ActivityState::Paused});
    if (!rows) {
        m_error = rows.error().message;
        return tasks;
    }
    for (const auto &row : rows.value()) {
        auto t = m_activity->task(row.activityId);
        if (!t) {
            m_error = t.error().message;
            return {};
        }
        tasks.append(t.value());
    }
    return tasks;
}

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
    if (!q.exec("PRAGMA user_version") || !q.next() ||
        q.value(0).toInt() >= 3) {
        m_error = "Legacy task writes are disabled for runtime schema 3";
        q.finish();
        db.close();
        return false;
    }
    const QStringList statements{
        "PRAGMA busy_timeout=5000", "PRAGMA synchronous=FULL",
        "CREATE TABLE IF NOT EXISTS complex_tasks (id TEXT PRIMARY KEY, terminal INTEGER NOT NULL, updated_at TEXT NOT NULL, record_json TEXT NOT NULL)",
        "CREATE UNIQUE INDEX IF NOT EXISTS complex_tasks_single_unfinished ON complex_tasks(terminal) WHERE terminal=0",
        "CREATE TABLE IF NOT EXISTS complex_task_experience_outbox (task_id TEXT PRIMARY KEY, payload_json TEXT NOT NULL, delivered INTEGER NOT NULL DEFAULT 0)"};
    for (const auto& sql : statements) if (!q.exec(sql)) { m_error = q.lastError().text(); return false; }
    m_error.clear(); return true;
}

void TaskStore::close() {
    m_activity.reset();
    if (m_connectionName.isEmpty()) return;
    { auto db = QSqlDatabase::database(m_connectionName, false); db.close(); }
    QSqlDatabase::removeDatabase(m_connectionName); m_connectionName.clear();
}
bool TaskStore::isOpen() const {
    return m_activity ||
           (!m_connectionName.isEmpty() &&
            QSqlDatabase::database(m_connectionName, false).isOpen());
}
bool TaskStore::save(const TaskRecord &task, bool enqueue) {
    auto copy = task;
    return save(copy, enqueue);
}
bool TaskStore::save(TaskRecord &task, bool enqueue) {
    if (m_activity) {
        auto r = m_activity->saveTask(task, enqueue);
        if (!r)
            m_error = r.error().message;
        return r.isOk();
    }
    if (!isOpen()) { m_error = "Task database is unavailable"; return false; }
    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    if (!q.exec("BEGIN IMMEDIATE")) {
        m_error = q.lastError().text();
        return false;
    }
    if (!q.exec("PRAGMA user_version") || !q.next() ||
        q.value(0).toInt() >= 3) {
        m_error = "Legacy task writes are disabled";
        db.rollback();
        return false;
    }
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
    if (m_activity) {
        auto tasks = unfinishedTasks();
        return tasks.isEmpty() ? std::optional<TaskRecord>{}
                               : std::optional<TaskRecord>{tasks.first()};
    }
    if (!isOpen()) return {};
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    if (!q.exec("SELECT record_json FROM complex_tasks WHERE terminal=0 LIMIT 1")) { m_error = q.lastError().text(); return {}; }
    if (!q.next()) return {};
    return TaskRecord::fromJson(QJsonDocument::fromJson(q.value(0).toByteArray()).object());
}
std::optional<TaskRecord> TaskStore::find(const QString& id) const {
    if (m_activity) {
        auto r = m_activity->task(id);
        if (!r) {
            m_error = r.error().message;
            return {};
        }
        return r.value();
    }
    if (!isOpen()) return {};
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare("SELECT record_json FROM complex_tasks WHERE id=?"); q.addBindValue(id);
    if (!q.exec()) { m_error = q.lastError().text(); return {}; }
    if (!q.next()) return {};
    return TaskRecord::fromJson(QJsonDocument::fromJson(q.value(0).toByteArray()).object());
}
QJsonArray TaskStore::pendingExperiences() const {
    if (m_activity) {
        auto r = m_activity->pendingExperiences();
        if (!r) {
            m_error = r.error().message;
            return {};
        }
        return r.value();
    }
    QJsonArray result;
    if (!isOpen()) return result;
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    if (!q.exec("SELECT payload_json FROM complex_task_experience_outbox WHERE delivered=0 ORDER BY rowid")) { m_error = q.lastError().text(); return result; }
    while (q.next()) result.append(QJsonDocument::fromJson(q.value(0).toByteArray()).object());
    return result;
}
bool TaskStore::acknowledgeExperience(const QString& id) {
    if (m_activity) {
        auto r = m_activity->acknowledgeExperience(id);
        if (!r)
            m_error = r.error().message;
        return r.isOk();
    }
    if (!isOpen()) return false;
    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    if (!q.exec("BEGIN IMMEDIATE"))
        return false;
    if (!q.exec("PRAGMA user_version") || !q.next() ||
        q.value(0).toInt() >= 3) {
        db.rollback();
        m_error = "Legacy task writes are disabled";
        return false;
    }
    q.prepare("UPDATE complex_task_experience_outbox SET delivered=1 WHERE task_id=?"); q.addBindValue(id);
    if (!q.exec()) {
        m_error = q.lastError().text();
        db.rollback();
        return false;
    }
    const bool updated = q.numRowsAffected() == 1;
    if (!updated) {
        db.rollback();
        return false;
    }
    return db.commit();
}
