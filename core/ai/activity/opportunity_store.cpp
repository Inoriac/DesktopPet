#include "opportunity_store.h"
#include "ai/event/runtime_unit_of_work.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <algorithm>
#include <limits>

namespace {
template <class T> Result<T, DomainError> fail(const QString &code, const QString &message) {
    return Result<T, DomainError>::failure(domainError(code, message));
}
QString json(const QJsonObject &o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
struct State {
    qint64 revision = 0;
    QJsonObject control;
};
Result<State, DomainError> read(QSqlDatabase db, const QString &profile) {
    QSqlQuery q(db);
    q.prepare("SELECT revision,control_json FROM agent_runtime_state WHERE profile_id=?");
    q.addBindValue(profile);
    if (!q.exec())
        return fail<State>("STORE_UNAVAILABLE", q.lastError().text());
    if (!q.next())
        return Result<State, DomainError>::success({});
    QJsonParseError e;
    auto d = QJsonDocument::fromJson(q.value(1).toByteArray(), &e);
    if (e.error != QJsonParseError::NoError || !d.isObject())
        return fail<State>("STORE_UNAVAILABLE", "Invalid runtime control state");
    const auto o = d.object();
    for (const auto &key : QStringList{"hourlyBudget", "selectionReservation", "inputWatermarks",
                                       "observedWatermarks", "opportunityClock", "autonomyConfig"})
        if (o.contains(key) && !o[key].isObject())
            return fail<State>("STORE_UNAVAILABLE", "Invalid runtime control object: " + key);
    for (const auto &key : QStringList{"paused", "autonomyEnabled", "configuredPaused"})
        if (o.contains(key) && !o[key].isBool())
            return fail<State>("STORE_UNAVAILABLE", "Invalid runtime control flag");
    auto integer = [](const QJsonValue &v) {
        return v.isDouble() && v.toDouble() == double(v.toInteger()) && v.toInteger() >= 0;
    };
    for (const auto &key :
         QStringList{"hostRevision", "lastDecisionLogical", "nextConsiderLogical", "backoff"})
        if (o.contains(key) && !integer(o[key]))
            return fail<State>("STORE_UNAVAILABLE", "Invalid runtime counter");
    const auto budget = o.value("hourlyBudget").toObject();
    for (const auto &key : QStringList{"attempts", "activeSamples"})
        if (budget.contains(key) && !budget[key].isObject())
            return fail<State>("STORE_UNAVAILABLE", "Invalid budget object");
    if (budget.contains("activeSegments") && !budget.value("activeSegments").isArray())
        return fail<State>("STORE_UNAVAILABLE", "Invalid active segments");
    for (const auto &v : budget.value("attempts").toObject()) {
        if (!v.isObject())
            return fail<State>("STORE_UNAVAILABLE", "Invalid attempt");
        const auto a = v.toObject();
        for (const auto &key : QStringList{"tokens", "cost", "at"})
            if (!integer(a[key]))
                return fail<State>("STORE_UNAVAILABLE", "Invalid attempt counter");
        if (!a.value("settled").isBool() || !a.value("usageKnown").isBool() ||
            (a["purpose"] != "selection" && a["purpose"] != "activity"))
            return fail<State>("STORE_UNAVAILABLE", "Invalid attempt identity");
    }
    for (const auto &v : budget.value("activeSamples").toObject())
        if (!integer(v))
            return fail<State>("STORE_UNAVAILABLE", "Invalid active sample");
    for (const auto &v : budget.value("activeSegments").toArray())
        if (!v.isObject() || !integer(v.toObject()["ms"]) || !integer(v.toObject()["at"]))
            return fail<State>("STORE_UNAVAILABLE", "Invalid active interval");
    const auto reservation = o.value("selectionReservation").toObject();
    if (!reservation.isEmpty() &&
        (reservation.value("ticketId").toString().isEmpty() ||
         reservation.value("clockSessionId").toString().isEmpty() ||
         !integer(reservation["monotonicDeadline"]) || !integer(reservation["hostRevision"]) ||
         !reservation.value("inputWatermark").isObject()))
        return fail<State>("STORE_UNAVAILABLE", "Invalid selection reservation");
    return Result<State, DomainError>::success({q.value(0).toLongLong(), o});
}
Result<void, DomainError> write(QSqlDatabase db, const QString &profile, State &s) {
    for (const auto &key : QStringList{"selectionReservation", "inputWatermarks",
                                       "observedWatermarks", "opportunityClock", "hostRevision",
                                       "lastDecisionLogical", "nextConsiderLogical", "backoff"})
        if (s.control.value(key).isNull() || s.control.value(key).isUndefined())
            s.control.remove(key);
    QSqlQuery q(db);
    if (s.revision == 0)
        q.prepare(
            "INSERT INTO agent_runtime_state(profile_id,revision,control_json) VALUES(?,1,?)");
    else
        q.prepare("UPDATE agent_runtime_state SET control_json=?,revision=revision+1 WHERE "
                  "profile_id=? AND revision=?");
    if (s.revision == 0) {
        q.addBindValue(profile);
        q.addBindValue(json(s.control));
    } else {
        q.addBindValue(json(s.control));
        q.addBindValue(profile);
        q.addBindValue(s.revision);
    }
    if (!q.exec())
        return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
    if (q.numRowsAffected() != 1)
        return fail<void>("STALE_REVISION", "Runtime state changed");
    ++s.revision;
    return Result<void, DomainError>::success();
}
struct Transaction {
    std::unique_ptr<RuntimeUnitOfWork> unit;
    QSqlDatabase db;
    DomainError error;
    Transaction(const QString &path) {
        auto u = SqliteRuntimeUnitOfWork::begin(path);
        if (!u) {
            error = domainError("STORE_UNAVAILABLE", u.error().message);
            return;
        }
        unit = u.takeValue();
        db = QSqlDatabase::database(unit->connectionName());
        QSqlQuery q(db);
        if (!q.exec("UPDATE agent_runtime_state SET revision=revision WHERE 0"))
            error = domainError("STORE_UNAVAILABLE", q.lastError().text());
    }
    ~Transaction() {
        db = QSqlDatabase();
    }
    bool ok() const {
        return unit && error.code.isEmpty();
    }
    Result<void, DomainError> commit() {
        auto r = unit->commit();
        return r ? Result<void, DomainError>::success()
                 : fail<void>("STORE_UNAVAILABLE", r.error().message);
    }
};
bool validClock(const OpportunityClock &c) {
    return c.utc.isValid() && c.monotonicMs >= 0 && !c.sessionId.isEmpty();
}
// Logical UTC advances at most by monotonic elapsed time. Across a restart we
// retain the previous anchor; a backward wall clock freezes eviction.
qint64 logicalNow(const QJsonObject &control, const OpportunityClock &c) {
    auto anchor = control.value("opportunityClock").toObject();
    if (anchor.isEmpty())
        return c.utc.toMSecsSinceEpoch();
    const qint64 old = anchor.value("logical").toInteger();
    if (c.utc.toMSecsSinceEpoch() < anchor.value("wall").toInteger())
        return old;
    if (anchor.value("session").toString() != c.sessionId)
        return old;
    return old + qMax<qint64>(0, c.monotonicMs - anchor.value("monotonic").toInteger());
}
void anchor(QJsonObject &control, const OpportunityClock &c, qint64 logical) {
    const auto old = control.value("opportunityClock").toObject();
    control["opportunityClock"] =
        QJsonObject{{"logical", logical},
                    {"wall", qMax(c.utc.toMSecsSinceEpoch(), old.value("wall").toInteger())},
                    {"monotonic", c.monotonicMs},
                    {"session", c.sessionId}};
}
QJsonObject configJson(const AutonomyConfig &c) {
    return {{"memoryAccess", QJsonObject{{"owner", c.memoryAccessOwner},
                                         {"conversationIds",
                                          QJsonArray::fromStringList(
                                              c.memoryAccessConversationIds)}}},
            {"maxSelectionCallsPerHour", c.maxSelectionCallsPerHour},
            {"maxActiveMsPerHour", c.maxActiveMsPerHour},
            {"maxTokensPerHour", c.maxTokensPerHour},
            {"maxEstimatedCostMicrosPerHour", c.maxEstimatedCostMicrosPerHour},
            {"unknownAttemptCostMicros", c.unknownAttemptCostMicros},
            {"minDecisionIntervalMs", c.minDecisionIntervalMs},
            {"noOpBackoffMs", c.noOpBackoffMs},
            {"maxNoOpBackoffMs", c.maxNoOpBackoffMs},
            {"ticketTtlMs", c.ticketTtlMs}};
}
bool expired(const QJsonObject &reservation, const OpportunityClock &clock) {
    if (reservation.isEmpty())
        return false;
    const auto t = OpportunityTicket::fromJson(reservation);
    return t.clockSessionId != clock.sessionId || clock.monotonicMs >= t.monotonicDeadline;
}
qint64 saturatingAdd(qint64 a, qint64 b) {
    return a > std::numeric_limits<qint64>::max() - b ? std::numeric_limits<qint64>::max() : a + b;
}
struct Totals {
    qint64 tokens = 0, cost = 0, active = 0;
    int selection = 0;
};
Totals totals(const QJsonObject &control, qint64 now) {
    Totals t;
    const auto b = control.value("hourlyBudget").toObject();
    const auto attempts = b.value("attempts").toObject();
    for (auto it = attempts.begin(); it != attempts.end(); ++it) {
        const auto a = it.value().toObject();
        if (a.value("at").toInteger() <= now - 3600000 && a.value("settled").toBool())
            continue;
        t.tokens = saturatingAdd(t.tokens, a.value("tokens").toInteger());
        t.cost = saturatingAdd(t.cost, a.value("cost").toInteger());
        if (a["purpose"] == "selection")
            ++t.selection;
    }
    for (const auto &value : b.value("activeSegments").toArray()) {
        auto a = value.toObject();
        if (a.value("at").toInteger() > now - 3600000)
            t.active = saturatingAdd(t.active, a.value("ms").toInteger());
    }
    return t;
}
Result<void, DomainError> archiveAttempts(QSqlDatabase db, const QString &profile,
                                          QJsonObject &control, qint64 now) {
    auto budget = control.value("hourlyBudget").toObject(),
         attempts = budget.value("attempts").toObject();
    for (auto it = attempts.begin(); it != attempts.end();) {
        const auto a = it.value().toObject();
        if (!a.value("settled").toBool() || a.value("at").toInteger() > now - 3600000) {
            ++it;
            continue;
        }
        QSqlQuery q(db);
        q.prepare("INSERT INTO agent_runtime_migrations(profile_id,migration_id,cursor_json,state) "
                  "VALUES(?,?,?,'completed') ON CONFLICT(profile_id,migration_id) DO NOTHING");
        q.addBindValue(profile);
        q.addBindValue("model-attempt:" + it.key());
        q.addBindValue(json(a));
        if (!q.exec())
            return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
        it = attempts.erase(it);
    }
    budget["attempts"] = attempts;
    QJsonArray active;
    for (const auto &v : budget.value("activeSegments").toArray())
        if (v.toObject()["at"].toInteger() > now - 3600000)
            active.append(v);
    if (budget.contains("activeSegments"))
        budget["activeSegments"] = active;
    control["hourlyBudget"] = budget;
    return Result<void, DomainError>::success();
}
Result<void, DomainError> commitState(Transaction &tx, const QString &profile, State &s,
                                      const OpportunityClock &clock) {
    auto archived = archiveAttempts(tx.db, profile, s.control, logicalNow(s.control, clock));
    if (!archived)
        return archived;
    anchor(s.control, clock, logicalNow(s.control, clock));
    auto w = write(tx.db, profile, s);
    return w ? tx.commit() : w;
}
bool sameInput(const QJsonObject &a, const QJsonObject &b) {
    for (auto it = b.begin(); it != b.end(); ++it)
        if (a[it.key()] != it.value())
            return false;
    return true;
}
QJsonObject mergeInput(QJsonObject previous, const QJsonObject &incoming) {
    for (auto it = incoming.begin(); it != incoming.end(); ++it)
        previous[it.key()] = it.value();
    return previous;
}
Result<void, DomainError> budgetError(qint64 logical, const OpportunityClock &clock) {
    Q_UNUSED(logical);
    return Result<void, DomainError>::failure(
        domainError("BUDGET_EXHAUSTED", "Autonomous hourly budget exhausted",
                    {{"nextWindowAt", clock.utc.addSecs(3600).toString(Qt::ISODateWithMs)}}));
}
} // namespace
OpportunityStore::OpportunityStore(QString p, QString path)
    : m_profileId(std::move(p)), m_databasePath(std::move(path)) {}
OpportunityClock OpportunityStore::systemClock() {
    static const QString session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    static const QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return {QDateTime::currentDateTimeUtc(), timer.elapsed(), session};
}
Result<QJsonObject, DomainError> OpportunityStore::snapshot() const {
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<QJsonObject, DomainError>::failure(tx.error);
    auto s = read(tx.db, m_profileId);
    if (!s)
        return Result<QJsonObject, DomainError>::failure(s.error());
    auto output = s.value().control;
    output["runtimeRevision"] = s.value().revision;
    return Result<QJsonObject, DomainError>::success(output);
}
Result<void, DomainError> OpportunityStore::configure(const AutonomyConfig &config,
                                                      const OpportunityClock &clock) {
    if (!validClock(clock))
        return fail<void>("INVALID_INPUT", "Invalid host clock");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<void, DomainError>::failure(tx.error);
    auto loaded = read(tx.db, m_profileId);
    if (!loaded)
        return Result<void, DomainError>::failure(loaded.error());
    auto s = loaded.value();
    if (!m_hostEpoch.isEmpty() && m_hostEpoch != s.control.value("hostEpoch").toString())
        return fail<void>("STALE_CONTEXT", "Host instance retired");
    const auto previous = s.control;
    s.control["autonomyEnabled"] = config.enabled;
    if (!s.control.contains("paused") ||
        (s.control.contains("configuredPaused") &&
         s.control.value("configuredPaused").toBool() != config.paused))
        s.control["paused"] = config.paused;
    s.control["configuredPaused"] = config.paused;
    s.control["autonomyConfig"] = configJson(config);
    if (previous.value("autonomyConfig").toObject() !=
            s.control.value("autonomyConfig").toObject() ||
        !config.enabled || s.control.value("paused").toBool() ||
        expired(s.control.value("selectionReservation").toObject(), clock))
        s.control.remove("selectionReservation");
    const bool restart =
        s.control.value("opportunityClock").toObject()["session"].toString() != clock.sessionId;
    if (restart) {
        auto b = s.control.value("hourlyBudget").toObject(),
             attempts = b.value("attempts").toObject();
        for (auto it = attempts.begin(); it != attempts.end(); ++it) {
            auto a = it.value().toObject();
            a["settled"] = true;
            it.value() = a;
        }
        b["attempts"] = attempts;
        s.control["hourlyBudget"] = b;
    }
    if (previous == s.control && !restart)
        return Result<void, DomainError>::success();
    return commitState(tx, m_profileId, s, clock);
}
Result<void, DomainError> OpportunityStore::setPaused(bool paused, const OpportunityClock &clock) {
    if (!validClock(clock))
        return fail<void>("INVALID_INPUT", "Invalid host clock");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<void, DomainError>::failure(tx.error);
    auto loaded = read(tx.db, m_profileId);
    if (!loaded)
        return Result<void, DomainError>::failure(loaded.error());
    auto s = loaded.value();
    if (!m_hostEpoch.isEmpty() && m_hostEpoch != s.control.value("hostEpoch").toString())
        return fail<void>("STALE_CONTEXT", "Host instance retired");
    s.control["paused"] = paused;
    if (paused)
        s.control.remove("selectionReservation");
    return commitState(tx, m_profileId, s, clock);
}
Result<void, DomainError> OpportunityStore::publishReadiness(const OpportunitySignal &signal) {
    if (!validClock(signal.clock))
        return fail<void>("INVALID_INPUT", "Invalid host clock");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<void, DomainError>::failure(tx.error);
    auto loaded = read(tx.db, m_profileId);
    if (!loaded)
        return Result<void, DomainError>::failure(loaded.error());
    auto s = loaded.value();
    if (signal.readiness.hostEpoch != s.control.value("hostEpoch").toString() ||
        qint64(signal.readiness.hostRevision) < s.control.value("hostRevision").toInteger())
        return fail<void>("STALE_CONTEXT", "Old host readiness");
    auto before = s.control;
    s.control["hostRevision"] = qint64(signal.readiness.hostRevision);
    s.control["inputWatermarks"] =
        mergeInput(s.control.value("inputWatermarks").toObject(), signal.inputWatermark);
    auto t = OpportunityTicket::fromJson(s.control.value("selectionReservation").toObject());
    if (!evaluateIdle(signal.readiness).canDeliberate ||
        expired(s.control.value("selectionReservation").toObject(), signal.clock) ||
        (!t.ticketId.isEmpty() &&
         (t.hostRevision != signal.readiness.hostRevision ||
          t.inputWatermark != s.control.value("inputWatermarks").toObject())))
        s.control.remove("selectionReservation");
    if (before == s.control)
        return Result<void, DomainError>::success();
    return commitState(tx, m_profileId, s, signal.clock);
}
Result<OpportunityTicket, DomainError>
OpportunityStore::considerOpportunity(const OpportunitySignal &signal) {
    if (!validClock(signal.clock))
        return fail<OpportunityTicket>("INVALID_INPUT", "Invalid host clock");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<OpportunityTicket, DomainError>::failure(tx.error);
    auto loaded = read(tx.db, m_profileId);
    if (!loaded)
        return Result<OpportunityTicket, DomainError>::failure(loaded.error());
    auto s = loaded.value();
    if ((signal.expectedRuntimeRevision >= 0 && signal.expectedRuntimeRevision != s.revision) ||
        signal.readiness.hostEpoch != s.control.value("hostEpoch").toString() ||
        qint64(signal.readiness.hostRevision) < s.control.value("hostRevision").toInteger())
        return fail<OpportunityTicket>("STALE_REVISION", "Old opportunity snapshot");
    auto readiness = signal.readiness;
    auto defer = [&](const QString &message) {
        if (expired(s.control.value("selectionReservation").toObject(), signal.clock)) {
            s.control.remove("selectionReservation");
            auto saved = commitState(tx, m_profileId, s, signal.clock);
            if (!saved)
                return Result<OpportunityTicket, DomainError>::failure(saved.error());
        }
        return fail<OpportunityTicket>("OPPORTUNITY_DEFERRED", message);
    };
    readiness.autonomyEnabled = s.control.value("autonomyEnabled").toBool();
    readiness.paused = readiness.paused || s.control.value("paused").toBool();
    auto reservation = s.control.value("selectionReservation").toObject();
    if (expired(reservation, signal.clock))
        reservation = {};
    readiness.selectionReservation = reservation.value("ticketId").toString();
    QSqlQuery q(tx.db);
    q.prepare("SELECT id FROM agent_activities WHERE profile_id=? AND focus_owned=1");
    q.addBindValue(m_profileId);
    if (!q.exec())
        return fail<OpportunityTicket>("STORE_UNAVAILABLE", q.lastError().text());
    if (q.next())
        readiness.foregroundActivity = q.value(0).toString();
    auto decision = evaluateIdle(readiness);
    if (!decision.canDeliberate)
        return defer(decision.reason);
    const auto config = AutonomyConfig::fromJson(s.control.value("autonomyConfig").toObject());
    const qint64 now = logicalNow(s.control, signal.clock);
    const bool changed =
        !sameInput(s.control.value("observedWatermarks").toObject(), signal.inputWatermark);
    if (s.control.contains("lastDecisionLogical") &&
        now - s.control.value("lastDecisionLogical").toInteger() < config.minDecisionIntervalMs)
        return defer("cooldown");
    if (!changed && now < s.control.value("nextConsiderLogical").toInteger())
        return defer("backoff");
    const auto used = totals(s.control, now);
    if (used.selection >= config.maxSelectionCallsPerHour ||
        used.tokens >= config.maxTokensPerHour ||
        used.cost >= config.maxEstimatedCostMicrosPerHour ||
        used.active >= config.maxActiveMsPerHour)
        return Result<OpportunityTicket, DomainError>::failure(
            budgetError(now, signal.clock).error());
    OpportunityTicket ticket;
    ticket.ticketId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ticket.profileId = m_profileId;
    ticket.hostEpoch = signal.readiness.hostEpoch;
    ticket.contextId = signal.contextId;
    ticket.eventIds = signal.eventIds;
    ticket.runtimeRevision = s.revision + 1;
    ticket.hostRevision = signal.readiness.hostRevision;
    ticket.inputWatermark =
        mergeInput(s.control.value("inputWatermarks").toObject(), signal.inputWatermark);
    ticket.expiresAt = signal.clock.utc.addMSecs(config.ticketTtlMs);
    ticket.clockSessionId = signal.clock.sessionId;
    ticket.monotonicDeadline = signal.clock.monotonicMs + config.ticketTtlMs;
    s.control["selectionReservation"] = ticket.toJson();
    s.control["hostRevision"] = qint64(ticket.hostRevision);
    s.control["inputWatermarks"] =
        mergeInput(s.control.value("inputWatermarks").toObject(), signal.inputWatermark);
    s.control["lastDecisionLogical"] = now;
    s.control["lastDecisionAt"] = signal.clock.utc.toString(Qt::ISODateWithMs);
    if (changed)
        s.control["backoff"] = 0;
    auto committed = commitState(tx, m_profileId, s, signal.clock);
    return committed ? Result<OpportunityTicket, DomainError>::success(ticket)
                     : Result<OpportunityTicket, DomainError>::failure(committed.error());
}
Result<void, DomainError> OpportunityStore::finishOpportunity(const OpportunityFinish &f) {
    if (!validClock(f.clock) ||
        (f.outcome != "wait" && f.outcome != "cancel" && f.outcome != "failure"))
        return fail<void>("INVALID_INPUT", "Invalid opportunity outcome");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<void, DomainError>::failure(tx.error);
    auto loaded = read(tx.db, m_profileId);
    if (!loaded)
        return Result<void, DomainError>::failure(loaded.error());
    auto s = loaded.value();
    auto reservation = s.control.value("selectionReservation").toObject();
    auto ticket = OpportunityTicket::fromJson(reservation);
    if (ticket.ticketId.isEmpty() || ticket.ticketId != f.ticketId ||
        expired(reservation, f.clock) || ticket.hostRevision != f.hostRevision ||
        ticket.inputWatermark != f.inputWatermark)
        return fail<void>("STALE_CONTEXT", "Opportunity is no longer current");
    s.control.remove("selectionReservation");
    if (f.outcome == "wait") {
        auto config = AutonomyConfig::fromJson(s.control.value("autonomyConfig").toObject());
        qint64 delay = qMin<qint64>(
            config.maxNoOpBackoffMs,
            qMax<qint64>(config.noOpBackoffMs, s.control.value("backoff").toInteger() * 2));
        if (f.nextConsiderAt.isValid())
            delay = std::clamp<qint64>(f.clock.utc.msecsTo(f.nextConsiderAt),
                                       config.minDecisionIntervalMs, config.maxNoOpBackoffMs);
        s.control["backoff"] = delay;
        s.control["nextConsiderLogical"] = logicalNow(s.control, f.clock) + delay;
        s.control["nextConsiderAt"] = f.clock.utc.addMSecs(delay).toString(Qt::ISODateWithMs);
        s.control["observedWatermarks"] =
            mergeInput(s.control.value("observedWatermarks").toObject(), f.inputWatermark);
    }
    s.control["lastDecision"] = QJsonObject{
        {"ticketId", f.ticketId}, {"outcome", f.outcome}, {"reason", f.reason.left(512)}};
    return commitState(tx, m_profileId, s, f.clock);
}
Result<ModelBudgetReservation, DomainError>
OpportunityStore::reserveModelAttempt(const ModelBudgetRequest &r) {
    if (r.profileId != m_profileId || r.attemptId.isEmpty() || !validClock(r.clock) ||
        r.inputBytes < 0 || r.inputBytes > 1000000000 || r.maxOutputTokens <= 0 ||
        r.maxOutputTokens > 1000000000 || (r.purpose != "selection" && r.purpose != "activity"))
        return fail<ModelBudgetReservation>("INVALID_INPUT", "Invalid host budget request");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<ModelBudgetReservation, DomainError>::failure(tx.error);
    auto loaded = read(tx.db, m_profileId);
    if (!loaded)
        return Result<ModelBudgetReservation, DomainError>::failure(loaded.error());
    auto s = loaded.value();
    if (!m_hostEpoch.isEmpty() && m_hostEpoch != s.control.value("hostEpoch").toString())
        return fail<ModelBudgetReservation>("STALE_CONTEXT", "Host instance retired");
    auto b = s.control.value("hourlyBudget").toObject(), attempts = b.value("attempts").toObject();
    if (attempts.contains(r.attemptId))
        return fail<ModelBudgetReservation>("STALE_REVISION", "Attempt id already reserved");
    QSqlQuery prior(tx.db);
    prior.prepare("SELECT 1 FROM agent_runtime_migrations WHERE profile_id=? AND migration_id=?");
    prior.addBindValue(m_profileId);
    prior.addBindValue("model-attempt:" + r.attemptId);
    if (!prior.exec())
        return fail<ModelBudgetReservation>("STORE_UNAVAILABLE", prior.lastError().text());
    if (prior.next())
        return fail<ModelBudgetReservation>("STALE_REVISION", "Attempt id already archived");
    if (!s.control.value("autonomyEnabled").toBool() || s.control.value("paused").toBool())
        return fail<ModelBudgetReservation>("OPPORTUNITY_DEFERRED", "Autonomy disabled or paused");
    if (r.purpose == "selection") {
        auto reservation = s.control.value("selectionReservation").toObject();
        if (r.ticketId.isEmpty() || reservation["ticketId"] != r.ticketId ||
            expired(reservation, r.clock))
            return fail<ModelBudgetReservation>("STALE_CONTEXT", "Selection ticket expired");
        int count = 0;
        for (const auto &a : attempts)
            if (a.toObject()["ticketId"] == r.ticketId)
                ++count;
        if (count >= 2)
            return fail<ModelBudgetReservation>("BUDGET_EXHAUSTED",
                                                "Selection attempt limit reached");
    } else {
        QSqlQuery q(tx.db);
        q.prepare("SELECT intent_json FROM agent_activities WHERE profile_id=? AND id=? AND "
                  "focus_owned=1");
        q.addBindValue(m_profileId);
        q.addBindValue(r.activityId);
        if (!q.exec())
            return fail<ModelBudgetReservation>("STORE_UNAVAILABLE", q.lastError().text());
        if (!q.next())
            return fail<ModelBudgetReservation>("STALE_CONTEXT",
                                                "Autonomous activity no longer running");
        auto intent = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
        if (intent["origin"] != "self" && intent["origin"] != "event")
            return fail<ModelBudgetReservation>("INVALID_INPUT", "Activity is not autonomous");
    }
    auto config = AutonomyConfig::fromJson(s.control.value("autonomyConfig").toObject());
    auto now = logicalNow(s.control, r.clock);
    auto used = totals(s.control, now);
    ModelBudgetReservation reservation{
        r.attemptId, r.inputBytes + 1024 + r.maxOutputTokens,
        qMax(r.routeEstimatedCostMicros, config.unknownAttemptCostMicros)};
    if (reservation.tokens > config.maxTokensPerHour - qMin(used.tokens, config.maxTokensPerHour) ||
        reservation.estimatedCostMicros >
            config.maxEstimatedCostMicrosPerHour -
                qMin(used.cost, config.maxEstimatedCostMicrosPerHour) ||
        used.active >= config.maxActiveMsPerHour ||
        (r.purpose == "selection" && used.selection >= config.maxSelectionCallsPerHour))
        return Result<ModelBudgetReservation, DomainError>::failure(
            budgetError(now, r.clock).error());
    attempts[r.attemptId] = QJsonObject{{"tokens", reservation.tokens},
                                        {"cost", reservation.estimatedCostMicros},
                                        {"at", now},
                                        {"purpose", r.purpose},
                                        {"ticketId", r.ticketId},
                                        {"activityId", r.activityId},
                                        {"settled", false},
                                        {"usageKnown", false}};
    b["attempts"] = attempts;
    s.control["hourlyBudget"] = b;
    auto committed = commitState(tx, m_profileId, s, r.clock);
    return committed ? Result<ModelBudgetReservation, DomainError>::success(reservation)
                     : Result<ModelBudgetReservation, DomainError>::failure(committed.error());
}
Result<void, DomainError> OpportunityStore::settleModelAttempt(const ModelBudgetSettlement &r) {
    if (r.profileId != m_profileId || !validClock(r.clock))
        return fail<void>("INVALID_INPUT", "Wrong settlement identity");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<void, DomainError>::failure(tx.error);
    auto loaded = read(tx.db, m_profileId);
    if (!loaded)
        return Result<void, DomainError>::failure(loaded.error());
    auto s = loaded.value();
    auto b = s.control.value("hourlyBudget").toObject(), attempts = b.value("attempts").toObject();
    const bool archived = !attempts.contains(r.attemptId);
    QJsonObject a = attempts[r.attemptId].toObject();
    if (archived) {
        QSqlQuery q(tx.db);
        q.prepare("SELECT cursor_json FROM agent_runtime_migrations WHERE profile_id=? AND "
                  "migration_id=?");
        q.addBindValue(m_profileId);
        q.addBindValue("model-attempt:" + r.attemptId);
        if (!q.exec())
            return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
        if (!q.next())
            return fail<void>("UNKNOWN_REFERENCE", "Unknown model attempt");
        QJsonParseError error;
        auto doc = QJsonDocument::fromJson(q.value(0).toByteArray(), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject())
            return fail<void>("STORE_UNAVAILABLE", "Invalid archived attempt");
        a = doc.object();
    }
    if (a.value("usageKnown").toBool())
        return Result<void, DomainError>::success();
    const bool nonnegative =
        r.usage.totalTokens >= 0 && r.usage.promptTokens >= 0 && r.usage.completionTokens >= 0;
    qint64 tokens = nonnegative ? r.usage.totalTokens : 0;
    if (tokens == 0 && r.usage.promptTokens > 0 && r.usage.completionTokens > 0 &&
        r.usage.promptTokens <= std::numeric_limits<qint64>::max() - r.usage.completionTokens)
        tokens = r.usage.promptTokens + r.usage.completionTokens;
    if (tokens > 0) {
        a["tokens"] = tokens;
        a["usageKnown"] = true;
    }
    a["settled"] = true;
    if (archived) {
        QSqlQuery q(tx.db);
        q.prepare("UPDATE agent_runtime_migrations SET cursor_json=? WHERE profile_id=? AND "
                  "migration_id=?");
        q.addBindValue(json(a));
        q.addBindValue(m_profileId);
        q.addBindValue("model-attempt:" + r.attemptId);
        if (!q.exec())
            return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
        return tx.commit();
    }
    attempts[r.attemptId] = a;
    b["attempts"] = attempts;
    s.control["hourlyBudget"] = b;
    return commitState(tx, m_profileId, s, r.clock);
}
Result<qint64, DomainError> OpportunityStore::accountActive(QSqlDatabase db, const QString &profile,
                                                            const QString &activity,
                                                            qint64 cumulative,
                                                            const OpportunityClock &clock) {
    auto loaded = read(db, profile);
    if (!loaded)
        return Result<qint64, DomainError>::failure(loaded.error());
    auto s = loaded.value();
    if (!s.control.contains("autonomyConfig"))
        return Result<qint64, DomainError>::success(std::numeric_limits<qint64>::max());
    auto config = AutonomyConfig::fromJson(s.control.value("autonomyConfig").toObject());
    auto b = s.control.value("hourlyBudget").toObject(),
         samples = b.value("activeSamples").toObject();
    qint64 previous = samples[activity].toInteger();
    if (!samples.contains(activity)) {
        QSqlQuery q(db);
        q.prepare("SELECT budget_json FROM agent_activities WHERE profile_id=? AND id=?");
        q.addBindValue(profile);
        q.addBindValue(activity);
        if (!q.exec())
            return fail<qint64>("STORE_UNAVAILABLE", q.lastError().text());
        if (q.next())
            previous = QJsonDocument::fromJson(q.value(0).toByteArray())
                           .object()["used"]
                           .toObject()["active_ms"]
                           .toInteger();
    }
    qint64 delta = qMax<qint64>(0, cumulative - previous);
    const qint64 now = logicalNow(s.control, clock);
    if (delta > 0) {
        QJsonArray segments;
        for (const auto &v : b.value("activeSegments").toArray())
            if (v.toObject()["at"].toInteger() + 3600000 > now)
                segments.append(v);
        if (!segments.isEmpty() && segments.last().toObject()["activityId"] == activity &&
            segments.last().toObject()["at"].toInteger() / 1000 == now / 1000) {
            auto tail = segments.last().toObject();
            tail["ms"] = tail.value("ms").toInteger() + delta;
            tail["at"] = now;
            segments.replace(segments.size() - 1, tail);
        } else
            segments.append(QJsonObject{{"at", now}, {"ms", delta}, {"activityId", activity}});
        samples[activity] = cumulative;
        b["activeSamples"] = samples;
        b["activeSegments"] = segments;
        s.control["hourlyBudget"] = b;
        auto archived = archiveAttempts(db, profile, s.control, now);
        if (!archived)
            return Result<qint64, DomainError>::failure(archived.error());
        anchor(s.control, clock, now);
        auto saved = write(db, profile, s);
        if (!saved)
            return Result<qint64, DomainError>::failure(saved.error());
    }
    return Result<qint64, DomainError>::success(
        qMax<qint64>(0, config.maxActiveMsPerHour - totals(s.control, now).active));
}

Result<qint64, DomainError>
OpportunityStore::remainingActiveMs(const OpportunityClock &clock) const {
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<qint64, DomainError>::failure(tx.error);
    auto loaded = read(tx.db, m_profileId);
    if (!loaded)
        return Result<qint64, DomainError>::failure(loaded.error());
    const auto &control = loaded.value().control;
    auto config = AutonomyConfig::fromJson(control.value("autonomyConfig").toObject());
    return Result<qint64, DomainError>::success(qMax<qint64>(
        0, config.maxActiveMsPerHour - totals(control, logicalNow(control, clock)).active));
}

Result<void, DomainError> OpportunityStore::bindHostEpoch(const QString &epoch,
                                                          const OpportunityClock &clock) {
    if (epoch.isEmpty() || !validClock(clock))
        return fail<void>("INVALID_INPUT", "Invalid host epoch");
    Transaction tx(m_databasePath);
    if (!tx.ok())
        return Result<void, DomainError>::failure(tx.error);
    auto loaded = read(tx.db, m_profileId);
    if (!loaded)
        return Result<void, DomainError>::failure(loaded.error());
    auto s = loaded.value();
    if (s.control.value("hostEpoch").toString() != epoch) {
        auto budget = s.control.value("hourlyBudget").toObject(),
             attempts = budget.value("attempts").toObject();
        for (auto it = attempts.begin(); it != attempts.end(); ++it) {
            auto attempt = it.value().toObject();
            attempt["settled"] = true;
            it.value() = attempt;
        }
        budget["attempts"] = attempts;
        s.control["hourlyBudget"] = budget;
    }
    s.control["hostEpoch"] = epoch;
    s.control["hostRevision"] = 0;
    s.control.remove("selectionReservation");
    auto committed = commitState(tx, m_profileId, s, clock);
    if (committed)
        m_hostEpoch = epoch;
    return committed;
}

Result<void, DomainError> OpportunityStore::consumeSelection(
    QSqlDatabase db, const QString &profile, const OpportunityTicket &ticket,
    const QString &decision, const OpportunityClock &clock) {
    auto loaded = read(db, profile);
    if (!loaded)
        return Result<void, DomainError>::failure(loaded.error());
    auto s = loaded.value();
    const auto reservation = s.control.value("selectionReservation").toObject();
    if (ticket.profileId != profile || ticket.ticketId.isEmpty() ||
        reservation != ticket.toJson() || expired(reservation, clock) ||
        s.control.value("hostEpoch").toString() != ticket.hostEpoch ||
        s.control.value("hostRevision").toInteger() !=
            qint64(ticket.hostRevision) ||
        s.control.value("inputWatermarks").toObject() !=
            ticket.inputWatermark ||
        s.control.value("paused").toBool() ||
        !s.control.value("autonomyEnabled").toBool())
        return fail<void>("STALE_CONTEXT", "Selection is no longer authorized");
    auto config =
        AutonomyConfig::fromJson(s.control.value("autonomyConfig").toObject());
    auto used = totals(s.control, logicalNow(s.control, clock));
    if (used.tokens >= config.maxTokensPerHour ||
        used.cost >= config.maxEstimatedCostMicrosPerHour ||
        used.active >= config.maxActiveMsPerHour)
        return fail<void>(
            "BUDGET_EXHAUSTED",
            "Autonomous execution budget exhausted after selection");
    s.control.remove("selectionReservation");
    s.control["lastDecision"] =
        QJsonObject{{"ticketId", ticket.ticketId}, {"outcome", decision}};
    s.control["observedWatermarks"] =
        mergeInput(s.control.value("observedWatermarks").toObject(),
                   ticket.inputWatermark);
    return write(db, profile, s);
}

Result<void, DomainError>
OpportunityStore::recordActualCheck(QSqlDatabase db, const QString &profile,
                                    const QString &activity,
                                    const QString &call, const QDateTime &at) {
    auto loaded = read(db, profile);
    if (!loaded)
        return Result<void, DomainError>::failure(loaded.error());
    auto s = loaded.value();
    auto previous = QDateTime::fromString(
        s.control.value("lastCheckedAt").toString(), Qt::ISODateWithMs);
    if (!previous.isValid() || at > previous) {
        s.control["lastCheckedAt"] = at.toUTC().toString(Qt::ISODateWithMs);
        s.control["lastCheck"] =
            QJsonObject{{"activityId", activity}, {"callId", call}};
    }
    return write(db, profile, s);
}

Result<void, DomainError>
OpportunityStore::validateMemoryScope(QSqlDatabase db, const QString &profile,
                                      const ContextSpec &context) {
    if (context.originKind != OriginKind::Internal)
        return Result<void, DomainError>::success();
    auto state = read(db, profile);
    if (!state)
        return Result<void, DomainError>::failure(state.error());
    auto config = AutonomyConfig::fromJson(
        state.value().control.value("autonomyConfig").toObject());
    if (context.allowOwnerMemory && !config.memoryAccessOwner)
        return fail<void>("PERMISSION_DENIED",
                          "Owner memory authorization was revoked");
    for (const auto &id : context.authorizedConversationIds) {
        if (!config.memoryAccessConversationIds.contains(id))
            return fail<void>("PERMISSION_DENIED",
                              "Conversation memory authorization was revoked");
        QSqlQuery q(db);
        q.prepare(
            "SELECT 1 FROM agent_conversations WHERE profile_id=? AND id=?");
        q.addBindValue(profile);
        q.addBindValue(id);
        if (!q.exec())
            return fail<void>("STORE_UNAVAILABLE", q.lastError().text());
        if (!q.next())
            return fail<void>("UNKNOWN_REFERENCE",
                              "Authorized conversation is unavailable");
    }
    return Result<void, DomainError>::success();
}
