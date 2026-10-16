#include "activity_store.h"
#include "ai/chat/conversation_store.h"
#include "ai/chat/conversation_types.h"
#include "ai/event/event_outbox.h"
#include "ai/event/event_schema_registry.h"
#include "ai/event/runtime_unit_of_work.h"
#include "ai/event/sqlite_event_repository.h"
#include "opportunity_store.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <limits>

namespace {
QString now() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
QString json(const QJsonObject &value) {
    return QString::fromUtf8(
        QJsonDocument(value).toJson(QJsonDocument::Compact));
}
QJsonObject object(const QVariant &value) {
    return QJsonDocument::fromJson(value.toByteArray()).object();
}
DomainError error(const QString &code, const QString &message) {
    return domainError(code, message);
}
DomainError storeError(const QString &message) {
    return error("STORE_UNAVAILABLE", message);
}
template <class T>
Result<T, DomainError> fail(const QString &code, const QString &message) {
    return Result<T, DomainError>::failure(error(code, message));
}
struct Transaction {
    std::unique_ptr<RuntimeUnitOfWork> unit;
    QSqlDatabase db;
    DomainError failure;
    Transaction(const QString &path, bool write = true) {
        auto started = SqliteRuntimeUnitOfWork::begin(path);
        if (!started) {
            failure = storeError(started.error().message);
            return;
        }
        unit = started.takeValue();
        db = QSqlDatabase::database(unit->connectionName());
        if (write) {
            QSqlQuery q(db);
            if (!q.exec(
                    "UPDATE agent_runtime_migrations SET updated_at=updated_at "
                    "WHERE 0"))
                failure = storeError(q.lastError().text());
        }
    }
    ~Transaction() { db = QSqlDatabase(); }
    bool ok() const { return unit && failure.code.isEmpty(); }
    Result<void, DomainError> commit() {
        auto r = unit->commit();
        return r ? Result<void, DomainError>::success()
                 : Result<void, DomainError>::failure(
                       storeError(r.error().message));
    }
};
bool terminalStatus(const QString &state) {
    return state == "Completed" || state == "Cancelled" || state == "Failed" ||
           state == "Declined";
}
ActivityState stateForTask(const TaskRecord &t) {
    if (t.status == "Declined")
        return ActivityState::Cancelled;
    if (t.status == "WaitingForUser")
        return ActivityState::WaitingUser;
    return activityStateFromName(t.status).value_or(ActivityState::Running);
}
Result<ActivitySnapshot, DomainError>
readActivity(QSqlDatabase db, const QString &profile, const QString &id) {
    QSqlQuery q(db);
    q.prepare("SELECT * FROM agent_activities WHERE profile_id=? AND id=?");
    q.addBindValue(profile);
    q.addBindValue(id);
    if (!q.exec())
        return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                      q.lastError().text());
    if (!q.next())
        return fail<ActivitySnapshot>("UNKNOWN_REFERENCE",
                                      "Activity does not exist");
    ActivitySnapshot a;
    a.profileId = profile;
    a.activityId = id;
    const auto state = activityStateFromName(q.value("state").toString());
    if (!state)
        return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                      "Invalid persisted activity state");
    a.state = *state;
    a.revision = q.value("revision").toLongLong();
    a.generation = q.value("generation").toULongLong();
    a.context = contextSpecFromJson(object(q.value("context_json")));
    a.intent = object(q.value("intent_json"));
    a.execution = object(q.value("execution_json"));
    a.waitingCondition = object(q.value("waiting_json"));
    const auto budget = object(q.value("budget_json"));
    if (!budget["limits"].isObject() || !budget["used"].isObject() ||
        a.context.profileId != profile)
        return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                      "Invalid activity scope or budget");
    const auto limits = budget["limits"].toObject(),
               used = budget["used"].toObject();
    for (const auto &key :
         QStringList{"max_tool_calls", "max_replans", "max_active_ms",
                     "model_timeout_ms", "tool_timeout_ms"})
        if (!limits[key].isDouble() ||
            limits[key].toDouble() != double(limits[key].toInteger()) ||
            limits[key].toInteger() < 0)
            return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                          "Invalid persisted budget limit");
    for (const auto &key : QStringList{"tool_calls", "replans", "active_ms"})
        if (!used[key].isDouble() ||
            used[key].toDouble() != double(used[key].toInteger()) ||
            used[key].toInteger() < 0)
            return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                          "Invalid persisted budget use");
    if (limits["max_active_ms"].toInteger() == 0 ||
        limits["model_timeout_ms"].toInteger() == 0 ||
        limits["tool_timeout_ms"].toInteger() == 0)
        return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                      "Invalid budget deadline");
    for (const auto &key : QStringList{"max_tool_calls", "max_replans",
                                       "model_timeout_ms", "tool_timeout_ms"})
        if (limits[key].toInteger() > std::numeric_limits<int>::max())
            return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                          "Budget limit is out of range");
    for (const auto &key : QStringList{"tool_calls", "replans"})
        if (used[key].toInteger() > std::numeric_limits<int>::max())
            return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                          "Budget use is out of range");
    a.budget = ActivityBudget::fromJson(budget);
    a.executionPhase = a.execution["executionPhase"].toString();
    a.checkpoint = a.execution["checkpoint"].toObject();
    a.pauseReason = a.execution["pauseReason"].toString();
    a.result = a.execution["result"].toString();
    return Result<ActivitySnapshot, DomainError>::success(a);
}
Result<void, DomainError> writeActivity(QSqlDatabase db, ActivitySnapshot &a,
                                        bool insert = false) {
    if (a.intent["origin"] == "self" || a.intent["origin"] == "event") {
        auto charged = OpportunityStore::accountActive(
            db, a.profileId, a.activityId, a.budget.activeMs, OpportunityStore::systemClock());
        if (!charged)
            return Result<void, DomainError>::failure(charged.error());
    }
    a.execution["executionPhase"] = a.executionPhase;
    a.execution["checkpoint"] = a.checkpoint;
    a.execution["pauseReason"] = a.pauseReason;
    a.execution["result"] = a.result;
    QSqlQuery q(db);
    if (insert)
        q.prepare(
            "INSERT INTO "
            "agent_activities(state,focus_owned,generation,context_json,"
            "intent_json,execution_json,waiting_json,budget_json,updated_at,"
            "profile_id,id,revision) VALUES(?,?,?,?,?,?,?,?,?,?,?,1)");
    else
        q.prepare(
            "UPDATE agent_activities SET "
            "state=?,focus_owned=?,generation=?,context_json=?,intent_json=?,"
            "execution_json=?,waiting_json=?,budget_json=?,updated_at=?,"
            "revision=revision+1 WHERE profile_id=? AND id=? AND revision=?");
    q.addBindValue(activityStateName(a.state));
    q.addBindValue(a.state == ActivityState::Running ? 1 : 0);
    q.addBindValue(qint64(a.generation));
    q.addBindValue(json(contextSpecToJson(a.context)));
    q.addBindValue(json(a.intent));
    q.addBindValue(json(a.execution));
    q.addBindValue(json(a.waitingCondition));
    q.addBindValue(json(a.budget.toJson()));
    q.addBindValue(now());
    q.addBindValue(a.profileId);
    q.addBindValue(a.activityId);
    if (!insert)
        q.addBindValue(a.revision);
    if (!q.exec())
        return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
    if (q.numRowsAffected() != 1)
        return fail<void>("STALE_REVISION",
                          "Activity changed before checkpoint commit");
    a.revision = insert ? 1 : a.revision + 1;
    return Result<void, DomainError>::success();
}
Result<ActionSnapshot, DomainError>
readAction(QSqlDatabase db, const QString &profile, const QString &id) {
    QSqlQuery q(db);
    q.prepare(
        "SELECT * FROM agent_action_calls WHERE profile_id=? AND call_id=?");
    q.addBindValue(profile);
    q.addBindValue(id);
    if (!q.exec())
        return fail<ActionSnapshot>("STORE_UNAVAILABLE", q.lastError().text());
    if (!q.next())
        return fail<ActionSnapshot>("UNKNOWN_REFERENCE",
                                    "Action does not exist");
    ActionSnapshot a;
    a.profileId = profile;
    a.callId = id;
    a.activityId = q.value("activity_id").toString();
    a.logicalKey = q.value("logical_key").toString();
    a.argumentRef = q.value("argument_ref").toString();
    a.generation = q.value("generation").toULongLong();
    a.capabilityRef =
        CapabilityRef::fromJson(object(q.value("capability_ref_json")));
    a.status = q.value("status").toString();
    a.result = object(q.value("result_json"));
    return Result<ActionSnapshot, DomainError>::success(a);
}
Result<void, DomainError> requireSettledActions(QSqlDatabase db,
                                                const QString &profile,
                                                const QString &activityId) {
    QSqlQuery query(db);
    query.prepare("SELECT 1 FROM agent_action_calls WHERE profile_id=? AND "
                  "activity_id=? AND status IN "
                  "('prepared','dispatched','accepted','unknown') LIMIT 1");
    query.addBindValue(profile);
    query.addBindValue(activityId);
    if (!query.exec())
        return fail<void>("STORE_UNAVAILABLE", query.lastError().text());
    if (query.next())
        return fail<void>("ACTION_OUTCOME_UNKNOWN",
                          "Completion requires every action to be settled");
    return Result<void, DomainError>::success();
}
Result<bool, DomainError> tableExists(QSqlDatabase db, const QString &table) {
    QSqlQuery q(db);
    q.prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
    q.addBindValue(table);
    if (!q.exec())
        return fail<bool>("STORE_UNAVAILABLE", q.lastError().text());
    return Result<bool, DomainError>::success(q.next());
}
Result<bool, DomainError> foregroundBusy(QSqlDatabase db,
                                         const QString &profile,
                                         const QString &except = {}) {
    QSqlQuery q(db);
    q.prepare("SELECT id FROM agent_activities WHERE profile_id=? AND "
              "focus_owned=1 AND id<>?");
    q.addBindValue(profile);
    q.addBindValue(except.isNull() ? QString("") : except);
    if (!q.exec())
        return fail<bool>("STORE_UNAVAILABLE", q.lastError().text());
    if (q.next())
        return Result<bool, DomainError>::success(true);
    q.prepare(
        "SELECT control_json FROM agent_runtime_state WHERE profile_id=?");
    q.addBindValue(profile);
    if (!q.exec())
        return fail<bool>("STORE_UNAVAILABLE", q.lastError().text());
    return Result<bool, DomainError>::success(
        q.next() &&
        !object(q.value(0))["selectionReservation"].toObject().isEmpty());
}
Result<void, DomainError> marker(QSqlDatabase db, const QString &profile,
                                 const QString &id,
                                 const QJsonObject &cursor = {}) {
    QSqlQuery q(db);
    q.prepare("INSERT INTO "
              "agent_runtime_migrations(profile_id,migration_id,cursor_json,"
              "state) VALUES(?,?,?,'completed') ON "
              "CONFLICT(profile_id,migration_id) DO UPDATE SET "
              "state='completed',cursor_json=excluded.cursor_json,updated_at="
              "excluded.updated_at");
    q.addBindValue(profile);
    q.addBindValue(id);
    q.addBindValue(json(cursor));
    if (!q.exec())
        return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
    return Result<void, DomainError>::success();
}
Result<bool, DomainError> marked(QSqlDatabase db, const QString &profile,
                                 const QString &id) {
    QSqlQuery q(db);
    q.prepare("SELECT 1 FROM agent_runtime_migrations WHERE profile_id=? AND "
              "migration_id=? AND state='completed'");
    q.addBindValue(profile);
    q.addBindValue(id);
    if (!q.exec())
        return fail<bool>("STORE_UNAVAILABLE", q.lastError().text());
    return Result<bool, DomainError>::success(q.next());
}
QJsonObject factualPayload(const TaskRecord &task) {
    return {{"task_id", task.id},
            {"profile_id", task.profileId},
            {"subject_id", task.subjectId},
            {"origin_kind", originKindToString(task.originKind)},
            {"context", contextSpecToJson(task.context)},
            {"goal", task.goal.left(2048)},
            {"status", task.status},
            {"result", task.result.left(4096)},
            {"reason", task.reason.left(2048)},
            {"personality_version", task.personalityVersion},
            {"evidence", task.evidence},
            {"initial_emotion", task.initialEmotion},
            {"current_emotion", task.currentEmotion},
            {"feedback_status",
             task.userFeedback.isEmpty() ? "unknown" : "provided"},
            {"user_feedback", task.userFeedback.left(2048)},
            {"confidence", task.status == "Completed" ? "verified" : "limited"},
            {"requirement_updates", task.requirementUpdates.left(4096)},
            {"created_at", task.updatedAt}};
}
QString readyId(const QString &profile, const QString &id) {
    return QUuid::createUuidV5(
               QUuid("66380c41-d69e-48bd-a6a3-1698b11eeace"),
               (profile + ":" + id + ":ExperienceReady").toUtf8())
        .toString(QUuid::WithoutBraces);
}
Result<void, DomainError> enqueueExperience(Transaction &tx,
                                            const QString &path,
                                            const TaskRecord &task) {
    auto consumed = marked(tx.db, task.profileId, "experience:" + task.id);
    if (!consumed)
        return Result<void, DomainError>::failure(consumed.error());
    if (consumed.value())
        return Result<void, DomainError>::success();
    QSqlQuery check(tx.db);
    auto old = tableExists(tx.db, "complex_task_experience_outbox");
    if (!old)
        return Result<void, DomainError>::failure(old.error());
    if (old.value()) {
        check.prepare(
            "SELECT 1 FROM complex_task_experience_outbox WHERE task_id=?");
        check.addBindValue(task.id);
        if (!check.exec())
            return fail<void>("STORE_UNAVAILABLE", check.lastError().text());
        if (check.next())
            return Result<void, DomainError>::success();
    }
    const auto id = readyId(task.profileId, task.id);
    check.prepare("SELECT 1 FROM event_outbox WHERE event_id=? UNION SELECT 1 "
                  "FROM event_log WHERE event_id IN (?,?)");
    check.addBindValue(id);
    check.addBindValue(id);
    check.addBindValue(task.id);
    if (!check.exec())
        return fail<void>("STORE_UNAVAILABLE", check.lastError().text());
    if (check.next())
        return Result<void, DomainError>::success();
    EventSchemaRegistry schemas;
    auto registered = registerBuiltInEventSchemas(schemas);
    if (!registered)
        return registered;
    schemas.freeze();
    SqliteEventOutbox outbox(path, &schemas, task.profileId);
    EventDraft draft;
    draft.eventId = id;
    draft.profileId = task.profileId;
    draft.type = "ExperienceReady";
    draft.source = "ActivityStore";
    draft.occurredAt = QDateTime::currentDateTimeUtc();
    draft.payload = factualPayload(task);
    auto result = outbox.enqueue(*tx.unit, draft);
    return result ? Result<void, DomainError>::success()
                  : fail<void>("STORE_UNAVAILABLE", result.error().message);
}
QJsonObject executionForTask(const TaskRecord &t,
                             const QJsonObject &previous = {}) {
    auto j = t.toJson();
    for (const auto &key :
         QStringList{"id", "profile_id", "status", "goal", "origin_kind",
                     "context", "activity_revision", "activity_generation",
                     "budget", "tool_calls", "replans", "active_ms"})
        j.remove(key);
    QJsonArray steps;
    for (const auto &value : t.steps) {
        auto s = value.toObject();
        for (const auto &key : QStringList{"status", "output", "error", "stage",
                                           "business_evidence", "arguments"})
            s.remove(key);
        steps.append(s);
    }
    j["steps"] = steps;
    j["argumentSlots"] = previous["argumentSlots"].toObject();
    return j;
}
Result<TaskRecord, DomainError> projectTask(const ActivitySnapshot &a,
                                            QSqlDatabase db) {
    auto t = TaskRecord::fromJson(a.execution);
    t.id = a.activityId;
    t.profileId = a.profileId;
    t.context = a.context;
    t.originKind = a.context.originKind;
    t.autonomous = a.intent["origin"] == "self" || a.intent["origin"] == "event";
    t.goal = a.intent["goal"].toString();
    t.activityRevision = a.revision;
    t.activityGeneration = a.generation;
    t.status = a.state == ActivityState::Running ? a.executionPhase
                                                 : activityStateName(a.state);
    if (t.status.isEmpty() || t.status == "checkpoint")
        t.status = "Received";
    if (a.state == ActivityState::WaitingUser)
        t.status = "WaitingForUser";
    if (a.state == ActivityState::Cancelled && a.executionPhase == "Declined")
        t.status = "Declined";
    t.reason = a.execution["reason"].toString(a.pauseReason);
    t.budget = a.budget.limits;
    t.toolCalls = a.budget.toolCalls;
    t.replans = a.budget.replans;
    t.activeMs = a.budget.activeMs;
    t.result = a.result;
    QSqlQuery q(db);
    q.prepare(
        "SELECT call_id,status,result_json,argument_ref,capability_ref_json "
        "FROM "
        "agent_action_calls WHERE profile_id=? AND activity_id=? ORDER BY "
        "created_at,call_id");
    q.addBindValue(a.profileId);
    q.addBindValue(a.activityId);
    if (!q.exec())
        return fail<TaskRecord>("STORE_UNAVAILABLE", q.lastError().text());
    while (q.next()) {
        const QString id = q.value(0).toString();
        QJsonObject step;
        int index = -1;
        for (int i = 0; i < t.steps.size(); ++i)
            if (t.steps[i].toObject()["id"].toString() == id) {
                step = t.steps[i].toObject();
                index = i;
                break;
            }
        const auto result = object(q.value(2));
        step["id"] = id;
        step["status"] = q.value(1).toString();
        step["read_only"] = result["readOnly"].toBool();
        auto receipt = result["receipt"].toObject();
        receipt.remove("business_evidence");
        step["output"] = receipt;
        step["stage"] = result["stage"];
        step["error"] = result["error"].toString();
        step["business_evidence"] = result["businessEvidence"].toBool(true);
        step["arguments"] = a.execution["argumentSlots"]
                                .toObject()[q.value(3).toString()]
                                .toObject();
        const auto ref = CapabilityRef::fromJson(object(q.value(4)));
        if (!step.contains("name"))
            step["name"] = ref.entryId;
        if (index < 0)
            t.steps.append(step);
        else
            t.steps[index] = step;
    }
    return Result<TaskRecord, DomainError>::success(t);
}
Result<void, DomainError> validateScope(QSqlDatabase db, ContextSpec &c) {
    if (c.conversationId.isEmpty())
        return Result<void, DomainError>::success();
    QSqlQuery q(db);
    q.prepare(
        "SELECT revision FROM agent_conversations WHERE profile_id=? AND id=?");
    q.addBindValue(c.profileId);
    q.addBindValue(c.conversationId);
    if (!q.exec())
        return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
    if (!q.next()) {
        if (c.originKind == OriginKind::LocalOwner &&
            c.conversationId == "local_owner")
            return Result<void, DomainError>::success();
        return fail<void>("UNKNOWN_REFERENCE", "Conversation does not exist");
    }
    c.conversationRevision = q.value(0).toLongLong();
    qint64 revision = 0;
    for (const auto &id : c.participantPersonIds) {
        q.prepare("SELECT revision FROM agent_persons WHERE profile_id=? AND "
                  "person_id=?");
        q.addBindValue(c.profileId);
        q.addBindValue(id);
        if (!q.exec())
            return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
        if (!q.next())
            return fail<void>("UNKNOWN_REFERENCE",
                              "Activity participant does not exist");
        revision += q.value(0).toLongLong();
    }
    c.identityRevision = revision;
    return Result<void, DomainError>::success();
}
} // namespace
ActivityStore::ActivityStore(QString profileId, QString path)
    : m_profileId(std::move(profileId)), m_databasePath(std::move(path)) {}
Result<void, DomainError> ActivityStore::initialize() {
    if (m_profileId.trimmed().isEmpty() || m_databasePath.isEmpty())
        return fail<void>("INVALID_INPUT",
                          "Activity profile and database are required");
    if (!QDir().mkpath(QFileInfo(m_databasePath).absolutePath()))
        return fail<void>("STORE_UNAVAILABLE",
                          "Cannot create runtime directory");
    SqliteEventRepository repository;
    auto opened = repository.open(m_databasePath);
    if (!opened)
        return fail<void>("STORE_UNAVAILABLE", opened.error().message);
    repository.close();
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<void, DomainError>::failure(tx.failure);
    auto migrated = marked(tx.db, m_profileId, "activity-v3");
    if (!migrated)
        return Result<void, DomainError>::failure(migrated.error());
    if (migrated.value())
        return tx.commit();
    auto legacyExists = tableExists(tx.db, "complex_tasks");
    if (!legacyExists)
        return Result<void, DomainError>::failure(legacyExists.error());
    if (legacyExists.value()) {
        QSqlQuery q(tx.db);
        if (!q.exec("SELECT record_json FROM complex_tasks ORDER BY id"))
            return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
        QList<TaskRecord> legacy;
        while (q.next()) {
            auto task = TaskRecord::fromJson(object(q.value(0)));
            if (task.profileId == m_profileId)
                legacy.append(task);
        }
        q.finish();
        for (auto &t : legacy) {
            auto existing = readActivity(tx.db, m_profileId, t.id);
            if (existing)
                continue;
            if (existing.error().code != "UNKNOWN_REFERENCE")
                return Result<void, DomainError>::failure(existing.error());
            ActivitySnapshot a;
            a.activityId = t.id;
            a.profileId = t.profileId;
            a.context = t.context;
            if (a.context.profileId.isEmpty()) {
                a.context.profileId = t.profileId;
                a.context.originKind = t.originKind;
                if (t.originKind == OriginKind::LocalOwner)
                    a.context = ContextSpec::localOwner(t.profileId);
            }
            a.context.activityId = t.id;
            a.state = t.terminal() ? stateForTask(t) : ActivityState::Paused;
            a.pauseReason = t.terminal() ? QString() : QString("restart");
            a.generation = t.activityGeneration + 1;
            a.executionPhase = t.status;
            a.execution = executionForTask(t);
            a.intent = {{"goal", t.goal}, {"origin", "user"}};
            a.budget = {t.budget, t.toolCalls, t.replans, t.activeMs};
            a.result = t.result;
            QJsonObject argumentSlots;
            for (const auto &value : t.steps) {
                const auto step = value.toObject();
                if (!step["id"].toString().isEmpty())
                    argumentSlots["action:" + step["id"].toString()] =
                        step["arguments"].toObject();
            }
            a.execution["argumentSlots"] = argumentSlots;
            auto saved = writeActivity(tx.db, a, true);
            if (!saved)
                return saved;
            for (const auto &value : t.steps) {
                auto s = value.toObject();
                const auto id = s["id"].toString();
                if (id.isEmpty())
                    continue;
                QString status = s["status"].toString();
                if (status == "dispatched" || status == "accepted" ||
                    status == "interrupted")
                    status = s["read_only"].toBool() ? "failed" : "unknown";
                if (status == "not_started")
                    status = "cancelled";
                if (!QStringList{"prepared", "unknown", "succeeded", "failed",
                                 "cancelled"}
                         .contains(status))
                    status = "unknown";
                QSqlQuery insert(tx.db);
                insert.prepare(
                    "INSERT INTO "
                    "agent_action_calls(profile_id,call_id,activity_id,"
                    "logical_key,generation,capability_ref_json,argument_"
                    "ref,status,result_json) VALUES(?,?,?,?,?,?,?,?,?)");
                insert.addBindValue(m_profileId);
                insert.addBindValue(id);
                insert.addBindValue(t.id);
                insert.addBindValue(id + ":" + s["signature"].toString(id));
                insert.addBindValue(qint64(t.activityGeneration));
                insert.addBindValue(json(CapabilityRef{
                    "builtin", s["name"].toString(), "1", "invoke"}
                                             .toJson()));
                insert.addBindValue("action:" + id);
                insert.addBindValue(status);
                insert.addBindValue(json(QJsonObject{
                    {"readOnly", s["read_only"]},
                    {"receipt", s["output"]},
                    {"stage", status == "succeeded" ? "completed" : "unknown"},
                    {"businessEvidence",
                     s["business_evidence"].toBool(true)}}));
                if (!insert.exec())
                    return fail<void>("STORE_UNAVAILABLE",
                                      insert.lastError().text());
            }
        }
    }
    auto oldOutbox = tableExists(tx.db, "complex_task_experience_outbox");
    if (!oldOutbox)
        return Result<void, DomainError>::failure(oldOutbox.error());
    if (oldOutbox.value()) {
        QSqlQuery q(tx.db);
        if (!q.exec("SELECT task_id,payload_json FROM "
                    "complex_task_experience_outbox WHERE delivered=1"))
            return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
        while (q.next())
            if (object(q.value(1))["profile_id"].toString() == m_profileId) {
                auto r = marker(tx.db, m_profileId,
                                "experience:" + q.value(0).toString(),
                                {{"source", "legacy-outbox"}});
                if (!r)
                    return r;
            }
    }
    auto markedResult =
        marker(tx.db, m_profileId, "activity-v3", {{"schemaVersion", 3}});
    if (!markedResult)
        return markedResult;
    return tx.commit();
}
Result<ActivitySnapshot, DomainError>
ActivityStore::find(const QString &id) const {
    Transaction tx(m_databasePath, false);
    if (!tx.ok())
        return Result<ActivitySnapshot, DomainError>::failure(tx.failure);
    return readActivity(tx.db, m_profileId, id);
}
Result<QList<ActivitySnapshot>, DomainError>
ActivityStore::list(const QList<ActivityState> &states) const {
    Transaction tx(m_databasePath, false);
    if (!tx.ok())
        return Result<QList<ActivitySnapshot>, DomainError>::failure(
            tx.failure);
    QSqlQuery q(tx.db);
    q.prepare("SELECT id FROM agent_activities WHERE profile_id=? ORDER BY "
              "updated_at DESC,id");
    q.addBindValue(m_profileId);
    if (!q.exec())
        return fail<QList<ActivitySnapshot>>("STORE_UNAVAILABLE",
                                             q.lastError().text());
    QList<ActivitySnapshot> rows;
    while (q.next()) {
        auto a = readActivity(tx.db, m_profileId, q.value(0).toString());
        if (!a)
            return Result<QList<ActivitySnapshot>, DomainError>::failure(
                a.error());
        if (states.isEmpty() || states.contains(a.value().state))
            rows.append(a.value());
    }
    return Result<QList<ActivitySnapshot>, DomainError>::success(rows);
}
Result<ActivitySnapshot, DomainError>
ActivityStore::startActivity(const ActivityIntent &intent) {
    if (intent.context.profileId != m_profileId ||
        intent.goal.trimmed().isEmpty() || intent.goal.size() > 16000 ||
        intent.budget.maxToolCalls < 0 || intent.budget.maxReplans < 0 ||
        intent.budget.maxActiveMs <= 0 || intent.budget.modelTimeoutMs <= 0 ||
        intent.budget.toolTimeoutMs <= 0)
        return fail<ActivitySnapshot>("INVALID_INPUT",
                                      "Invalid activity intent or budget");
    if (!intent.capabilityRefs.isEmpty()) {
        if (!m_capabilityValidator)
            return fail<ActivitySnapshot>(
                "DEPENDENCY_UNAVAILABLE",
                "Capability validator is unavailable");
        auto checked =
            m_capabilityValidator(intent.capabilityRefs, intent.context);
        if (!checked)
            return Result<ActivitySnapshot, DomainError>::failure(
                checked.error());
    }
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<ActivitySnapshot, DomainError>::failure(tx.failure);
    auto busy = foregroundBusy(tx.db, m_profileId);
    if (!busy)
        return Result<ActivitySnapshot, DomainError>::failure(busy.error());
    ActivitySnapshot a;
    a.activityId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    a.profileId = m_profileId;
    a.context = intent.context;
    a.context.activityId = a.activityId;
    a.intent = intent.toJson();
    a.execution["created_at"] = now();
    a.budget.limits = intent.budget;
    if (intent.origin == "self" || intent.origin == "event") {
        auto remaining = OpportunityStore::accountActive(tx.db, m_profileId, a.activityId, 0,
                                                         OpportunityStore::systemClock());
        if (!remaining)
            return Result<ActivitySnapshot, DomainError>::failure(remaining.error());
        if (remaining.value() == 0)
            return fail<ActivitySnapshot>("BUDGET_EXHAUSTED", "Autonomous active budget exhausted");
    }
    a.state = busy.value() ? ActivityState::Ready : ActivityState::Running;
    a.executionPhase = "checkpoint";
    auto checked = validateScope(tx.db, a.context);
    if (!checked)
        return Result<ActivitySnapshot, DomainError>::failure(checked.error());
    auto saved = writeActivity(tx.db, a, true);
    if (!saved)
        return Result<ActivitySnapshot, DomainError>::failure(saved.error());
    auto committed = tx.commit();
    return committed ? Result<ActivitySnapshot, DomainError>::success(a)
                     : Result<ActivitySnapshot, DomainError>::failure(
                           committed.error());
}
Result<ActivitySnapshot, DomainError>
ActivityStore::transitionActivity(const ActivityTransition &t) {
    if (t.profileId != m_profileId || t.activeMs < -1)
        return fail<ActivitySnapshot>("INVALID_INPUT",
                                      "Activity belongs to another profile");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<ActivitySnapshot, DomainError>::failure(tx.failure);
    auto loaded = readActivity(tx.db, m_profileId, t.activityId);
    if (!loaded)
        return loaded;
    auto a = loaded.value();
    if (a.revision != t.expectedRevision)
        return fail<ActivitySnapshot>("STALE_REVISION",
                                      "Activity revision changed");
    if (activityTerminal(a.state))
        return fail<ActivitySnapshot>("INVALID_INPUT",
                                      "Terminal activity cannot transition");
    if (t.targetState == ActivityState::Running &&
        a.state != ActivityState::Running)
        return fail<ActivitySnapshot>(
            "INVALID_INPUT", "Use authorized resume to acquire attention");
    if (a.state != ActivityState::Running &&
        t.targetState != ActivityState::Paused &&
        t.targetState != ActivityState::Cancelled &&
        t.targetState != ActivityState::Failed)
        return fail<ActivitySnapshot>(
            "INVALID_INPUT",
            "Paused or waiting activity requires authorized resume");
    if (t.targetState == ActivityState::WaitingExternal) {
        const auto w = t.waitingCondition;
        auto call = readAction(tx.db, m_profileId, w["callId"].toString());
        if (!call || call.value().activityId != a.activityId)
            return fail<ActivitySnapshot>(
                "INVALID_INPUT", "Waiting requires an archived action receipt");
        const auto output = call.value().result["receipt"].toObject();
        const auto receipt = output["receipt"].isObject()
                                 ? output["receipt"].toObject()
                                 : output;
        const auto kind = w["kind"].toString();
        if (kind == "message") {
            if (call.value().status != "succeeded" ||
                w["conversationId"].toString() != a.context.conversationId ||
                w["afterMessageId"].toString().isEmpty())
                return fail<ActivitySnapshot>(
                    "INVALID_INPUT", "Message waiting scope is invalid");
            QSqlQuery q(tx.db);
            q.prepare(
                "SELECT payload_json FROM agent_messages WHERE profile_id=? "
                "AND conversation_id=? AND id=? AND direction='outgoing' AND "
                "remote_id=?");
            q.addBindValue(m_profileId);
            q.addBindValue(a.context.conversationId);
            q.addBindValue(w["afterMessageId"].toString());
            q.addBindValue(receipt["remoteMessageId"].toString());
            if (!q.exec())
                return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                              q.lastError().text());
            if (!q.next() ||
                object(q.value(0))["senderKind"].toString() != "self")
                return fail<ActivitySnapshot>(
                    "INVALID_INPUT", "Sent message anchor is not verified");
        } else if (kind == "event") {
            if (call.value().status != "accepted" ||
                output["tracking"]
                    .toObject()["sourceId"]
                    .toString()
                    .isEmpty() ||
                output["tracking"]
                    .toObject()["remoteJobId"]
                    .toString()
                    .isEmpty() ||
                w["sourceId"] != output["tracking"].toObject()["sourceId"] ||
                w["remoteJobId"] !=
                    output["tracking"].toObject()["remoteJobId"])
                return fail<ActivitySnapshot>(
                    "INVALID_INPUT", "Accepted wait has no tracking source");
        } else
            return fail<ActivitySnapshot>("INVALID_INPUT",
                                          "Unsupported external waiting kind");
        if (w["resumeRule"].toString().isEmpty())
            return fail<ActivitySnapshot>("INVALID_INPUT",
                                          "Waiting resume rule is required");
        QSqlQuery pending(tx.db);
        pending.prepare(
            "SELECT 1 FROM agent_action_calls WHERE profile_id=? AND "
            "activity_id=? AND call_id<>? AND status IN "
            "('prepared','dispatched','accepted','unknown')");
        pending.addBindValue(m_profileId);
        pending.addBindValue(a.activityId);
        pending.addBindValue(call.value().callId);
        if (!pending.exec())
            return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                          pending.lastError().text());
        if (pending.next())
            return fail<ActivitySnapshot>(
                "INVALID_INPUT",
                "Other action results still require foreground");
        a.waitingCondition = w;
        a.waitingCondition["since"] = now();
    }
    if (t.targetState == ActivityState::Completed) {
        auto settled = requireSettledActions(tx.db, m_profileId, a.activityId);
        if (!settled)
            return Result<ActivitySnapshot, DomainError>::failure(
                settled.error());
        if (t.evidenceRefs.isEmpty() || t.reason.startsWith("MODEL_"))
            return fail<ActivitySnapshot>(
                "INVALID_INPUT", "Completion requires verified evidence");
        for (const auto &id : t.evidenceRefs) {
            auto call = readAction(tx.db, m_profileId, id);
            if (!call || call.value().activityId != a.activityId ||
                call.value().status != "succeeded")
                return fail<ActivitySnapshot>(
                    "INVALID_INPUT",
                    "Completion evidence is not a successful action");
            if (t.emptyCheck) {
                const auto output = call.value().result["receipt"].toObject();
                const auto receipt = output.contains("data")
                                         ? output["data"].toObject()
                                     : output["receipt"].isObject()
                                         ? output["receipt"].toObject()
                                         : output;
                const bool empty = receipt["empty"].isBool() &&
                                   receipt["empty"].toBool() &&
                                   receipt["items"].isArray() &&
                                   receipt["items"].toArray().isEmpty();
                if (!call.value().result["readOnly"].toBool() || !empty)
                    return fail<ActivitySnapshot>(
                        "INVALID_INPUT",
                        "An empty check requires actual empty read evidence");
            }
        }
    }
    if (a.state == ActivityState::Running &&
        t.targetState != ActivityState::Running)
        ++a.generation;
    a.state = t.targetState;
    a.result = t.result;
    if (!t.executionPhase.isEmpty())
        a.executionPhase = t.executionPhase;
    if (!t.checkpoint.isEmpty())
        a.checkpoint = t.checkpoint;
    a.pauseReason = t.reason;
    a.budget.activeMs = qMax(a.budget.activeMs, t.activeMs);
    if (t.activeMs >= 0)
        a.checkpoint["active_ms"] = a.budget.activeMs;
    a.execution["reason"] = t.reason;
    if (a.state == ActivityState::Paused && a.pauseReason.isEmpty())
        a.pauseReason = "user";
    auto saved = writeActivity(tx.db, a);
    if (!saved)
        return Result<ActivitySnapshot, DomainError>::failure(saved.error());
    if (activityTerminal(a.state) && !t.emptyCheck) {
        auto projected = projectTask(a, tx.db);
        if (!projected)
            return Result<ActivitySnapshot, DomainError>::failure(
                projected.error());
        auto task = projected.value();
        task.updatedAt = now();
        task.evidence = QJsonArray::fromStringList(t.evidenceRefs);
        auto e = enqueueExperience(tx, m_databasePath, task);
        if (!e)
            return Result<ActivitySnapshot, DomainError>::failure(e.error());
    }
    auto committed = tx.commit();
    return committed ? Result<ActivitySnapshot, DomainError>::success(a)
                     : Result<ActivitySnapshot, DomainError>::failure(
                           committed.error());
}
Result<ActivitySnapshot, DomainError>
ActivityStore::resumeActivity(const ResumeActivityRequest &r) {
    if (r.profileId != m_profileId || r.foregroundActiveMs < -1)
        return fail<ActivitySnapshot>("INVALID_INPUT",
                                      "Resume belongs to another profile");
    auto before = find(r.activityId);
    if (!before)
        return before;
    QList<CapabilityRef> refs;
    for (const auto &ref : before.value().intent["capabilityRefs"].toArray())
        refs.append(CapabilityRef::fromJson(ref.toObject()));
    ContextSpec fresh = before.value().context;
    if (!fresh.conversationId.isEmpty()) {
        ConversationStore conversations(m_profileId, m_databasePath);
        auto current = conversations.currentContext(fresh.conversationId);
        if (!current) {
            if (fresh.originKind != OriginKind::LocalOwner ||
                fresh.conversationId != "local_owner" ||
                current.error().code != "UNKNOWN_REFERENCE")
                return Result<ActivitySnapshot, DomainError>::failure(
                    current.error());
        } else {
            auto c = current.value();
            c.activityId = fresh.activityId;
            fresh = c;
        }
    }
    if (!refs.isEmpty()) {
        if (!m_capabilityValidator)
            return fail<ActivitySnapshot>("DEPENDENCY_UNAVAILABLE",
                                          "Capability validation is required");
        auto checked = m_capabilityValidator(refs, fresh);
        if (!checked)
            return Result<ActivitySnapshot, DomainError>::failure(
                checked.error());
    }
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<ActivitySnapshot, DomainError>::failure(tx.failure);
    auto loaded = readActivity(tx.db, m_profileId, r.activityId);
    if (!loaded)
        return loaded;
    auto a = loaded.value();
    if (a.revision != r.expectedRevision)
        return fail<ActivitySnapshot>("STALE_REVISION",
                                      "Resume revision changed");
    if (activityTerminal(a.state) || a.state == ActivityState::Running)
        return fail<ActivitySnapshot>("INVALID_INPUT",
                                      "Activity cannot resume");
    if (r.resumeActor != ResumeActor::User) {
        if ((r.resumeActor == ResumeActor::Scheduler &&
             a.waitingCondition["resumeRule"] != "scheduler" &&
             a.waitingCondition["resumeRule"] != "event_or_scheduler") ||
            a.state != ActivityState::WaitingExternal ||
            a.waitingCondition["resumeRule"].toString() == "user" ||
            r.triggerRef.isEmpty())
            return fail<ActivitySnapshot>("INVALID_INPUT",
                                          "This pause requires user authority");
        QSqlQuery q(tx.db);
        q.prepare(
            "SELECT type,payload_json FROM event_log WHERE profile_id=? AND "
            "event_id=?");
        q.addBindValue(m_profileId);
        q.addBindValue(r.triggerRef);
        if (!q.exec())
            return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                          q.lastError().text());
        if (!q.next())
            return fail<ActivitySnapshot>("UNKNOWN_REFERENCE",
                                          "Resume trigger has not committed");
        const auto type = q.value(0).toString();
        const auto payload = object(q.value(1));
        if (a.waitingCondition["kind"] == "message") {
            if (type != "MessageObserved" ||
                payload["conversation_id"].toString() !=
                    a.waitingCondition["conversationId"].toString() ||
                payload["sender_kind"] == "self" ||
                payload["historical_import"].toBool())
                return fail<ActivitySnapshot>(
                    "INVALID_INPUT", "Trigger does not match message wait");
            q.prepare(
                "SELECT m.rowid FROM agent_messages m WHERE m.profile_id=? AND "
                "m.conversation_id=? AND m.id=? AND m.direction='incoming' AND "
                "m.rowid>(SELECT rowid FROM agent_messages WHERE profile_id=? "
                "AND id=?)");
            q.addBindValue(m_profileId);
            q.addBindValue(a.context.conversationId);
            q.addBindValue(payload["message_id"].toString());
            q.addBindValue(m_profileId);
            q.addBindValue(a.waitingCondition["afterMessageId"].toString());
            if (!q.exec())
                return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                              q.lastError().text());
            if (!q.next())
                return fail<ActivitySnapshot>(
                    "INVALID_INPUT", "Reply does not follow sent message");
        } else if (type != "ActionCompleted" ||
                   payload["call_id"].toString() !=
                       a.waitingCondition["callId"].toString() ||
                   payload["source_id"].toString() !=
                       a.waitingCondition["sourceId"].toString() ||
                   payload["remote_job_id"] !=
                       a.waitingCondition["remoteJobId"] ||
                   payload["activity_id"].toString() != a.activityId)
            return fail<ActivitySnapshot>(
                "INVALID_INPUT", "Trigger does not match tracked action");
    }
    if (a.intent["origin"] == "self" || a.intent["origin"] == "event") {
        auto remaining = OpportunityStore::accountActive(
            tx.db, m_profileId, a.activityId, a.budget.activeMs, OpportunityStore::systemClock());
        if (!remaining)
            return Result<ActivitySnapshot, DomainError>::failure(remaining.error());
        if (remaining.value() == 0)
            return fail<ActivitySnapshot>("BUDGET_EXHAUSTED", "Autonomous active budget exhausted");
    }
    if (a.budget.activeMs >= a.budget.limits.maxActiveMs ||
        a.budget.toolCalls >= a.budget.limits.maxToolCalls)
        return fail<ActivitySnapshot>("BUDGET_EXHAUSTED",
                                      "Activity has no remaining budget");
    QSqlQuery unresolved(tx.db);
    unresolved.prepare(
        "SELECT 1 FROM agent_action_calls WHERE profile_id=? AND activity_id=? "
        "AND status IN ('dispatched','accepted','unknown')");
    unresolved.addBindValue(m_profileId);
    unresolved.addBindValue(a.activityId);
    if (!unresolved.exec())
        return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                      unresolved.lastError().text());
    if (unresolved.next())
        return fail<ActivitySnapshot>(
            "ACTION_OUTCOME_UNKNOWN",
            "Resolve the previous action before resuming");
    auto rechecked = fresh;
    auto checked = validateScope(tx.db, rechecked);
    if (!checked)
        return Result<ActivitySnapshot, DomainError>::failure(checked.error());
    if (rechecked.conversationRevision != fresh.conversationRevision ||
        rechecked.identityRevision != fresh.identityRevision)
        return fail<ActivitySnapshot>(
            "STALE_CONTEXT", "Conversation changed during resume validation");
    a.context = fresh;
    auto busy = foregroundBusy(tx.db, m_profileId, a.activityId);
    if (!busy)
        return Result<ActivitySnapshot, DomainError>::failure(busy.error());
    if (busy.value()) {
        if (r.expectedForegroundId.isEmpty())
            return fail<ActivitySnapshot>("STALE_REVISION",
                                          "Foreground is occupied");
        auto foreground =
            readActivity(tx.db, m_profileId, r.expectedForegroundId);
        if (!foreground || foreground.value().state != ActivityState::Running ||
            foreground.value().revision != r.expectedForegroundRevision)
            return fail<ActivitySnapshot>("STALE_REVISION",
                                          "Foreground changed");
        auto f = foreground.value();
        unresolved.prepare(
            "SELECT 1 FROM agent_action_calls WHERE profile_id=? AND "
            "activity_id=? "
            "AND status IN ('prepared','dispatched','accepted','unknown')");
        unresolved.addBindValue(m_profileId);
        unresolved.addBindValue(f.activityId);
        if (!unresolved.exec())
            return fail<ActivitySnapshot>("STORE_UNAVAILABLE",
                                          unresolved.lastError().text());
        if (unresolved.next() || f.executionPhase != "checkpoint")
            return fail<ActivitySnapshot>("INVALID_INPUT",
                                          "Foreground has unsettled execution");
        f.state = ActivityState::Paused;
        f.budget.activeMs = qMax(f.budget.activeMs, r.foregroundActiveMs);
        if (r.foregroundActiveMs >= 0)
            f.checkpoint["active_ms"] = f.budget.activeMs;
        f.pauseReason = "checkpoint";
        ++f.generation;
        auto saved = writeActivity(tx.db, f);
        if (!saved)
            return Result<ActivitySnapshot, DomainError>::failure(
                saved.error());
        auto remaining = foregroundBusy(tx.db, m_profileId, a.activityId);
        if (!remaining || remaining.value())
            return fail<ActivitySnapshot>(
                "STALE_REVISION", "Selection reservation owns attention");
    }
    a.state = ActivityState::Running;
    a.pauseReason.clear();
    ++a.generation;
    a.waitingCondition = {};
    auto saved = writeActivity(tx.db, a);
    if (!saved)
        return Result<ActivitySnapshot, DomainError>::failure(saved.error());
    auto committed = tx.commit();
    return committed ? Result<ActivitySnapshot, DomainError>::success(a)
                     : Result<ActivitySnapshot, DomainError>::failure(
                           committed.error());
}
Result<ActionSnapshot, DomainError>
ActivityStore::prepareAction(const ActionPreparation &p) {
    if (p.profileId != m_profileId || p.logicalStepId.isEmpty() ||
        p.capabilityRef.entryId.isEmpty() ||
        p.capabilityRef.pluginId.isEmpty() ||
        p.capabilityRef.entryVersion.isEmpty())
        return fail<ActionSnapshot>("INVALID_INPUT", "Invalid action identity");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<ActionSnapshot, DomainError>::failure(tx.failure);
    auto loaded = readActivity(tx.db, m_profileId, p.activityId);
    if (!loaded)
        return Result<ActionSnapshot, DomainError>::failure(loaded.error());
    auto a = loaded.value();
    if (a.intent["origin"] == "self" || a.intent["origin"] == "event") {
        auto remaining = OpportunityStore::accountActive(
            tx.db, m_profileId, a.activityId, a.budget.activeMs, OpportunityStore::systemClock());
        if (!remaining)
            return Result<ActionSnapshot, DomainError>::failure(remaining.error());
        if (remaining.value() == 0)
            return fail<ActionSnapshot>("BUDGET_EXHAUSTED", "Autonomous active budget exhausted");
    }
    if (a.revision != p.expectedRevision || a.generation != p.generation)
        return fail<ActionSnapshot>("STALE_REVISION", "Action context changed");
    if (a.state != ActivityState::Running)
        return fail<ActionSnapshot>("INVALID_INPUT",
                                    "Action requires foreground ownership");
    const auto digest = QString::fromLatin1(
        QCryptographicHash::hash(
            (p.capabilityRef.key() + ":" + json(p.arguments)).toUtf8(),
            QCryptographicHash::Sha256)
            .toHex());
    auto logical = p.readOnly ? p.logicalStepId + ":" + digest : digest;
    QSqlQuery q(tx.db);
    q.prepare("SELECT call_id FROM agent_action_calls WHERE profile_id=? AND "
              "activity_id=? AND logical_key=?");
    q.addBindValue(m_profileId);
    q.addBindValue(a.activityId);
    q.addBindValue(logical);
    if (!q.exec())
        return fail<ActionSnapshot>("STORE_UNAVAILABLE", q.lastError().text());
    if (q.next()) {
        auto existing = readAction(tx.db, m_profileId, q.value(0).toString());
        if (!existing)
            return existing;
        const auto proposedId = p.logicalStepId.startsWith(a.activityId + ":")
                                    ? p.logicalStepId
                                    : a.activityId + ":" + p.logicalStepId;
        if (p.readOnly || existing.value().callId == proposedId ||
            (existing.value().status != "failed" &&
             existing.value().status != "cancelled"))
            return existing;
        logical = p.logicalStepId + ":" + digest;
        auto retried = readAction(tx.db, m_profileId, proposedId);
        if (retried)
            return retried;
        if (retried.error().code != "UNKNOWN_REFERENCE")
            return retried;
    }
    if (!p.readOnly) {
        q.prepare(
            "SELECT call_id FROM agent_action_calls WHERE profile_id=? AND "
            "activity_id=? AND status IN "
            "('unknown','dispatched','accepted','succeeded')");
        q.addBindValue(m_profileId);
        q.addBindValue(a.activityId);
        if (!q.exec())
            return fail<ActionSnapshot>("STORE_UNAVAILABLE",
                                        q.lastError().text());
        while (q.next()) {
            auto previous =
                readAction(tx.db, m_profileId, q.value(0).toString());
            if (!previous)
                return previous;
            if (previous.value().capabilityRef == p.capabilityRef &&
                a.execution["argumentSlots"]
                        .toObject()[previous.value().argumentRef]
                        .toObject() == p.arguments)
                return previous;
        }
    }
    if (a.budget.toolCalls >= a.budget.limits.maxToolCalls ||
        a.budget.activeMs >= a.budget.limits.maxActiveMs)
        return fail<ActionSnapshot>("BUDGET_EXHAUSTED",
                                    "Action budget exhausted");
    ActionSnapshot call;
    call.profileId = m_profileId;
    call.activityId = a.activityId;
    call.callId = p.logicalStepId.startsWith(a.activityId + ":")
                      ? p.logicalStepId
                      : a.activityId + ":" + p.logicalStepId;
    call.logicalKey = logical;
    call.argumentRef = "action:" + call.callId;
    call.generation = a.generation;
    call.capabilityRef = p.capabilityRef;
    call.status = "prepared";
    call.result = {{"readOnly", p.readOnly}};
    q.prepare("INSERT INTO "
              "agent_action_calls(profile_id,call_id,activity_id,logical_key,"
              "generation,capability_ref_json,argument_ref,status,result_json) "
              "VALUES(?,?,?,?,?,?,?,'prepared',?)");
    q.addBindValue(m_profileId);
    q.addBindValue(call.callId);
    q.addBindValue(call.activityId);
    q.addBindValue(logical);
    q.addBindValue(qint64(call.generation));
    q.addBindValue(json(call.capabilityRef.toJson()));
    q.addBindValue(call.argumentRef);
    q.addBindValue(json(call.result));
    if (!q.exec())
        return fail<ActionSnapshot>("STORE_UNAVAILABLE", q.lastError().text());
    auto argumentSlots = a.execution["argumentSlots"].toObject();
    argumentSlots[call.argumentRef] = p.arguments;
    a.execution["argumentSlots"] = argumentSlots;
    // Preparation does not change semantic activity revision; CAS already holds
    // writer.
    q.prepare("UPDATE agent_activities SET execution_json=? WHERE profile_id=? "
              "AND id=?");
    q.addBindValue(json(a.execution));
    q.addBindValue(m_profileId);
    q.addBindValue(a.activityId);
    if (!q.exec())
        return fail<ActionSnapshot>("STORE_UNAVAILABLE", q.lastError().text());
    auto committed = tx.commit();
    return committed ? Result<ActionSnapshot, DomainError>::success(call)
                     : Result<ActionSnapshot, DomainError>::failure(
                           committed.error());
}
Result<ActionSnapshot, DomainError>
ActivityStore::markActionDispatched(const ActionDispatch &d) {
    if (d.profileId != m_profileId)
        return fail<ActionSnapshot>("INVALID_INPUT", "Action profile mismatch");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<ActionSnapshot, DomainError>::failure(tx.failure);
    auto loaded = readActivity(tx.db, m_profileId, d.activityId);
    if (!loaded)
        return Result<ActionSnapshot, DomainError>::failure(loaded.error());
    auto a = loaded.value();
    if (a.intent["origin"] == "self" || a.intent["origin"] == "event") {
        auto remaining = OpportunityStore::accountActive(
            tx.db, m_profileId, a.activityId, a.budget.activeMs, OpportunityStore::systemClock());
        if (!remaining)
            return Result<ActionSnapshot, DomainError>::failure(remaining.error());
        if (remaining.value() == 0)
            return fail<ActionSnapshot>("BUDGET_EXHAUSTED", "Autonomous active budget exhausted");
    }
    if (a.revision != d.expectedRevision || a.generation != d.generation)
        return fail<ActionSnapshot>("STALE_REVISION",
                                    "Dispatch context changed");
    if (a.state != ActivityState::Running)
        return fail<ActionSnapshot>("INVALID_INPUT",
                                    "Dispatch requires foreground");
    auto call = readAction(tx.db, m_profileId, d.callId);
    if (!call)
        return call;
    if (call.value().activityId != a.activityId ||
        call.value().generation != a.generation ||
        call.value().status != "prepared")
        return fail<ActionSnapshot>("INVALID_INPUT",
                                    "Call is not a dispatchable preparation");
    if (a.budget.toolCalls >= a.budget.limits.maxToolCalls ||
        a.budget.activeMs >= a.budget.limits.maxActiveMs)
        return fail<ActionSnapshot>("BUDGET_EXHAUSTED",
                                    "Dispatch budget exhausted");
    QSqlQuery q(tx.db);
    q.prepare("UPDATE agent_action_calls SET status='dispatched',updated_at=? "
              "WHERE profile_id=? AND call_id=? AND status='prepared'");
    q.addBindValue(now());
    q.addBindValue(m_profileId);
    q.addBindValue(d.callId);
    if (!q.exec() || q.numRowsAffected() != 1)
        return fail<ActionSnapshot>("STORE_UNAVAILABLE", q.lastError().text());
    ++a.budget.toolCalls;
    q.prepare("UPDATE agent_activities SET budget_json=? WHERE profile_id=? "
              "AND id=?");
    q.addBindValue(json(a.budget.toJson()));
    q.addBindValue(m_profileId);
    q.addBindValue(a.activityId);
    if (!q.exec())
        return fail<ActionSnapshot>("STORE_UNAVAILABLE", q.lastError().text());
    auto committed = tx.commit();
    if (!committed)
        return Result<ActionSnapshot, DomainError>::failure(committed.error());
    call.value().status = "dispatched";
    return call;
}
Result<ActionSnapshot, DomainError>
ActivityStore::findAction(const QString &id) const {
    Transaction tx(m_databasePath, false);
    if (!tx.ok())
        return Result<ActionSnapshot, DomainError>::failure(tx.failure);
    return readAction(tx.db, m_profileId, id);
}
Result<void, DomainError>
ActivityStore::observeAction(const ActionObservation &o) {
    if (o.profileId != m_profileId || !o.observedAt.isValid())
        return fail<void>("INVALID_INPUT", "Invalid action observation");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<void, DomainError>::failure(tx.failure);
    auto loaded = readAction(tx.db, m_profileId, o.callId);
    if (!loaded)
        return Result<void, DomainError>::failure(loaded.error());
    auto c = loaded.value();
    if (c.activityId != o.activityId || c.generation != o.dispatchGeneration)
        return fail<void>("STALE_CONTEXT",
                          "Receipt does not belong to this dispatch");
    if (c.status == "succeeded" || c.status == "failed" ||
        c.status == "cancelled")
        return tx.commit();
    if (c.status == "prepared" && o.outcome != ActionOutcome::NotExecuted)
        return fail<void>("INVALID_INPUT",
                          "Unsent action cannot have an execution receipt");
    const auto tracking = c.result["receipt"].toObject()["tracking"].toObject();
    const bool trackedCompletion =
        o.outcome == ActionOutcome::Succeeded && !tracking.isEmpty();
    const auto actual = o.receipt["receipt"].isObject()
                            ? o.receipt["receipt"].toObject()
                            : o.receipt;
    if (trackedCompletion && (tracking["sourceId"].toString().isEmpty() ||
                              tracking["remoteJobId"].toString().isEmpty() ||
                              actual["sourceId"] != tracking["sourceId"] ||
                              actual["remoteJobId"] != tracking["remoteJobId"]))
        return fail<void>(
            "INVALID_INPUT",
            "Completion does not match the original accepted tracking");
    const QStringList states{"accepted", "succeeded", "failed", "unknown",
                             "cancelled"};
    c.status = states.at(int(o.outcome));
    c.result["receipt"] = o.receipt;
    c.result["observedAt"] = o.observedAt.toUTC().toString(Qt::ISODateWithMs);
    c.result["stage"] =
        o.outcome == ActionOutcome::Succeeded ? "completed" : c.status;
    c.result["businessEvidence"] = o.receipt["business_evidence"].toBool(true);
    QSqlQuery q(tx.db);
    q.prepare(
        "UPDATE agent_action_calls SET status=?,result_json=?,updated_at=? "
        "WHERE profile_id=? AND call_id=?");
    q.addBindValue(c.status);
    q.addBindValue(json(c.result));
    q.addBindValue(now());
    q.addBindValue(m_profileId);
    q.addBindValue(c.callId);
    if (!q.exec())
        return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
    if (trackedCompletion) {
        EventSchemaRegistry schemas;
        auto registered = registerBuiltInEventSchemas(schemas);
        if (!registered)
            return registered;
        schemas.freeze();
        SqliteEventOutbox outbox(m_databasePath, &schemas, m_profileId);
        EventDraft event;
        event.eventId =
            QUuid::createUuidV5(QUuid("66380c41-d69e-48bd-a6a3-1698b11eeace"),
                                (m_profileId + ":" + o.activityId + ":" +
                                 o.callId + ":ActionCompleted")
                                    .toUtf8())
                .toString(QUuid::WithoutBraces);
        event.profileId = m_profileId;
        event.type = "ActionCompleted";
        event.source = "ActivityStore";
        event.occurredAt = o.observedAt;
        event.payload = {
            {"profile_id", m_profileId},
            {"activity_id", o.activityId},
            {"call_id", o.callId},
            {"source_id", tracking["sourceId"]},
            {"remote_job_id", tracking["remoteJobId"]},
            {"completed_at", o.observedAt.toUTC().toString(Qt::ISODateWithMs)}};
        auto enqueued = outbox.enqueue(*tx.unit, event);
        if (!enqueued)
            return fail<void>("STORE_UNAVAILABLE", enqueued.error().message);
    }
    return tx.commit();
}
Result<void, DomainError> ActivityStore::recoverAfterRestart() {
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<void, DomainError>::failure(tx.failure);
    QSqlQuery q(tx.db);
    q.prepare(
        "SELECT id FROM agent_activities WHERE profile_id=? AND state NOT "
        "IN ('Completed','Cancelled','Failed')");
    q.addBindValue(m_profileId);
    if (!q.exec())
        return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
    QStringList ids;
    while (q.next())
        ids.append(q.value(0).toString());
    q.finish();
    for (const auto &id : ids) {
        auto loaded = readActivity(tx.db, m_profileId, id);
        if (!loaded)
            return Result<void, DomainError>::failure(loaded.error());
        auto a = loaded.value();
        a.state = ActivityState::Paused;
        a.pauseReason = "restart";
        ++a.generation;
        auto saved = writeActivity(tx.db, a);
        if (!saved)
            return saved;
    }
    q.prepare(
        "SELECT call_id,status,result_json FROM agent_action_calls WHERE "
        "profile_id=? AND status IN ('prepared','dispatched','accepted')");
    q.addBindValue(m_profileId);
    if (!q.exec())
        return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
    QList<QPair<QString, QString>> calls;
    while (q.next())
        calls.append({q.value(0).toString(),
                      q.value(1).toString() == "prepared"       ? "cancelled"
                      : object(q.value(2))["readOnly"].toBool() ? "failed"
                                                                : "unknown"});
    q.finish();
    for (const auto &call : calls) {
        q.prepare("UPDATE agent_action_calls SET status=?,updated_at=? WHERE "
                  "profile_id=? AND call_id=?");
        q.addBindValue(call.second);
        q.addBindValue(now());
        q.addBindValue(m_profileId);
        q.addBindValue(call.first);
        if (!q.exec())
            return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
    }
    return tx.commit();
}
Result<void, DomainError> ActivityStore::saveTask(TaskRecord &task,
                                                  bool enqueue) {
    if (task.profileId != m_profileId || task.id.isEmpty())
        return fail<void>("INVALID_INPUT", "Task profile or id is invalid");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<void, DomainError>::failure(tx.failure);
    auto loaded = readActivity(tx.db, m_profileId, task.id);
    const bool insert = !loaded && loaded.error().code == "UNKNOWN_REFERENCE";
    if (!loaded && !insert)
        return Result<void, DomainError>::failure(loaded.error());
    ActivitySnapshot a = insert ? ActivitySnapshot{} : loaded.value();
    if (!insert && task.activityRevision != a.revision) {
        if (task.terminal() && stateForTask(task) == a.state &&
            task.result == a.result)
            return tx.commit();
        return fail<void>("STALE_REVISION", "Task checkpoint is stale");
    }
    if (!insert && activityTerminal(a.state) && stateForTask(task) != a.state)
        return fail<void>("INVALID_INPUT", "Cannot reactivate a terminal task");
    a.activityId = task.id;
    a.profileId = m_profileId;
    a.context = task.context;
    if (a.context.profileId.isEmpty()) {
        a.context.profileId = m_profileId;
        a.context.originKind = task.originKind;
        if (task.originKind == OriginKind::LocalOwner)
            a.context = ContextSpec::localOwner(m_profileId);
    }
    if (a.context.profileId != m_profileId)
        return fail<void>("INVALID_INPUT", "Task context profile mismatch");
    a.context.activityId = task.id;
    a.intent["goal"] = task.goal;
    if (!a.intent.contains("origin"))
        a.intent["origin"] = "user";
    if (task.status == "WaitingExternal" &&
        (!loaded || a.state != ActivityState::WaitingExternal))
        return fail<void>("INVALID_INPUT",
                          "Use validated external wait transition");
    if (task.status == "Completed" &&
        (!loaded || a.state != ActivityState::Completed)) {
        auto settled = requireSettledActions(tx.db, m_profileId, task.id);
        if (!settled)
            return settled;
        if (task.evidence.isEmpty())
            return fail<void>("INVALID_INPUT",
                              "Task completion needs verified evidence");
        for (const auto &ref : task.evidence) {
            if (ref.toString() == "user_input" &&
                task.originKind == OriginKind::LocalOwner && task.accepted)
                continue;
            auto call = readAction(tx.db, m_profileId, ref.toString());
            if (!call || call.value().activityId != task.id ||
                call.value().status != "succeeded")
                return fail<void>("INVALID_INPUT",
                                  "Task completion evidence is not verified");
        }
    }
    const auto target = stateForTask(task);
    if (!insert && target == ActivityState::Running &&
        a.state != ActivityState::Running)
        return fail<void>(
            "INVALID_INPUT",
            "Use authorized resume before saving a running checkpoint");
    if (target == ActivityState::Running && a.state != ActivityState::Running) {
        auto busy = foregroundBusy(tx.db, m_profileId, task.id);
        if (!busy)
            return Result<void, DomainError>::failure(busy.error());
        if (busy.value())
            return fail<void>("STALE_REVISION",
                              "Another activity owns foreground");
    }
    if (!insert && a.state == ActivityState::Running &&
        target != ActivityState::Running)
        ++a.generation;
    a.state = target;
    a.executionPhase = task.status;
    a.result = task.result;
    a.execution = executionForTask(task, a.execution);
    a.checkpoint = {{"steps", task.steps.size()}, {"plan", task.plan}};
    if (target == ActivityState::Paused)
        a.pauseReason = task.needsRevalidation ? "user" : "dependency";
    // Tool counts are exclusively charged at the dispatch boundary.
    a.budget.limits = task.budget;
    if (insert)
        a.budget.toolCalls = task.toolCalls;
    a.budget.replans = qMax(a.budget.replans, task.replans);
    a.budget.activeMs = qMax(a.budget.activeMs, task.activeMs);
    auto saved = writeActivity(tx.db, a, insert);
    if (!saved)
        return saved;
    if (enqueue) {
        auto e = enqueueExperience(tx, m_databasePath, task);
        if (!e)
            return e;
    }
    auto committed = tx.commit();
    if (committed) {
        task.activityRevision = a.revision;
        task.activityGeneration = a.generation;
        task.context = a.context;
        task.toolCalls = a.budget.toolCalls;
    }
    return committed;
}
Result<TaskRecord, DomainError> ActivityStore::task(const QString &id) const {
    Transaction tx(m_databasePath, false);
    if (!tx.ok())
        return Result<TaskRecord, DomainError>::failure(tx.failure);
    auto a = readActivity(tx.db, m_profileId, id);
    return a ? projectTask(a.value(), tx.db)
             : Result<TaskRecord, DomainError>::failure(a.error());
}
Result<QJsonArray, DomainError> ActivityStore::pendingExperiences() const {
    Transaction tx(m_databasePath, false);
    if (!tx.ok())
        return Result<QJsonArray, DomainError>::failure(tx.failure);
    QJsonArray values;
    QSet<QString> seen;
    QSqlQuery q(tx.db);
    auto append = [&](const QJsonObject &payload) -> Result<void, DomainError> {
        const auto id = payload["task_id"].toString();
        if (payload["profile_id"].toString() != m_profileId || id.isEmpty() ||
            seen.contains(id))
            return Result<void, DomainError>::success();
        auto consumed = marked(tx.db, m_profileId, "experience:" + id);
        if (!consumed)
            return Result<void, DomainError>::failure(consumed.error());
        if (!consumed.value()) {
            seen.insert(id);
            values.append(payload);
        }
        return Result<void, DomainError>::success();
    };
    if (!q.exec("SELECT payload_json FROM event_outbox UNION ALL SELECT "
                "payload_json FROM event_log WHERE type='ExperienceReady'"))
        return fail<QJsonArray>("STORE_UNAVAILABLE", q.lastError().text());
    while (q.next()) {
        auto envelope = object(q.value(0));
        auto payload = envelope.contains("type")
                           ? (envelope["type"] == "ExperienceReady"
                                  ? envelope["payload"].toObject()
                                  : QJsonObject{})
                           : envelope;
        auto r = append(payload);
        if (!r)
            return Result<QJsonArray, DomainError>::failure(r.error());
    }
    auto old = tableExists(tx.db, "complex_task_experience_outbox");
    if (!old)
        return Result<QJsonArray, DomainError>::failure(old.error());
    if (old.value()) {
        if (!q.exec(
                "SELECT payload_json FROM complex_task_experience_outbox WHERE "
                "delivered=0"))
            return fail<QJsonArray>("STORE_UNAVAILABLE", q.lastError().text());
        while (q.next()) {
            auto r = append(object(q.value(0)));
            if (!r)
                return Result<QJsonArray, DomainError>::failure(r.error());
        }
    }
    return Result<QJsonArray, DomainError>::success(values);
}
Result<void, DomainError>
ActivityStore::acknowledgeExperience(const QString &id) {
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<void, DomainError>::failure(tx.failure);
    auto r = marker(tx.db, m_profileId, "experience:" + id, {{"originId", id}});
    if (!r)
        return r;
    return tx.commit();
}

Result<qint64, DomainError> ActivityStore::remainingAutonomousActiveMs() const {
    return OpportunityStore(m_profileId, m_databasePath)
        .remainingActiveMs(OpportunityStore::systemClock());
}
