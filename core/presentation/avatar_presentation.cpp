#include "avatar_presentation.h"
#include <QDateTime>
#include <QRandomGenerator>
#include <QUuid>
#include <algorithm>
#include <cmath>

AvatarPresentation::AvatarPresentation(QObject* parent) : QObject(parent) {
    m_expiryTimer.setInterval(50);
    connect(&m_expiryTimer, &QTimer::timeout, this, &AvatarPresentation::expire);
}
AvatarPresentation::~AvatarPresentation() {
    if (m_backend) {
        disconnect(m_backend, nullptr, this, nullptr);
        m_backend->setDrawing(false);
        m_backend->stop(m_config["defaultState"].toString());
    }
}
QStringList AvatarPresentation::configure(const QJsonObject& config) {
    return serial([&] {
        QStringList errors, names;
        const auto states = config["states"].toArray();
        if (!config["states"].isArray() || states.isEmpty())
            errors << "states: expected a non-empty array";
        if (!config["transactions"].isArray())
            errors << "transactions: expected an array";
        for (const auto& value : states) {
            const auto s = value.toObject();
            const auto name = s["name"].toString();
            if (!value.isObject() || !s["name"].isString())
                errors << "states: expected an object with a string name";
            if (s.contains("loop") && !s["loop"].isBool())
                errors << name + ".loop: expected boolean";
            if (s.contains("clipOptions") && !s["clipOptions"].isArray())
                errors << name + ".clipOptions: expected array";
            if (name.isEmpty() || names.contains(name))
                errors << "states: empty or duplicate name: " + name;
            names << name;
            for (const QString field : {QString{"requestable"}, QString{"disabled"}})
                if (s.contains(field) && !s[field].isBool())
                    errors << name + "." + field + ": expected boolean";
            if (s.contains("interruption") && !s["interruption"].isString())
                errors << name + ".interruption: expected string";
            const QString rule = s["interruption"].toString("immediate");
            if (!QStringList{"immediate", "finish_cycle", "complete_transition"}.contains(rule))
                errors << name + ".interruption: unknown rule";
            for (const auto& a : s["clipOptions"].toArray()) {
                const auto option = a.toObject();
                const double weight = option["weight"].toDouble(1);
                if (!a.isObject() || (option.contains("weight") && !option["weight"].isDouble()) ||
                    !std::isfinite(weight) || weight <= 0)
                    errors << name + ".clipOptions.weight: expected positive finite number";
                if (option.contains("interruption") && !option["interruption"].isString())
                    errors << name + ".clipOptions.interruption: expected string";
                const auto override = option["interruption"].toString(rule);
                if (option["clipName"].toString().isEmpty() ||
                    !QStringList{"immediate", "finish_cycle", "complete_transition"}.contains(override))
                    errors << name + ".clipOptions: invalid action/rule";
            }
        }
        if (!names.contains(config["defaultState"].toString()))
            errors << "defaultState: unknown initial state";
        for (const auto& value : states)
            if (value.toObject()["name"] == config["defaultState"] &&
                value.toObject()["disabled"].toBool())
                errors << "defaultState: initial state is disabled";
        for (const auto& t : config["transactions"].toArray()) {
            const auto tr = t.toObject();
            if (!t.isObject() || (tr.contains("via") && !tr["via"].isArray()))
                errors << "transactions.via: expected an array";
            if ((!names.contains(tr["fromState"].toString()) && tr["fromState"] != "*") ||
                !names.contains(tr["toState"].toString()))
                errors << "transactions: unknown source/target";
            for (const auto& via : tr["via"].toArray()) {
                bool valid = false;
                for (const auto& s : states)
                    if (s.toObject()["name"] == via && !s.toObject()["loop"].toBool(true))
                        valid = true;
                if (!valid)
                    errors << "transactions.via: transition must reference a finite non-looping state: " +
                                  via.toString();
            }
            for (const auto& value : states) {
                const auto s = value.toObject();
                if (s["name"] != tr["fromState"] && tr["fromState"] != "*")
                    continue;
                bool requiresPath = s["interruption"] == "complete_transition";
                for (const auto& action : s["clipOptions"].toArray())
                    requiresPath |= action.toObject()["interruption"] == "complete_transition";
                if (requiresPath && tr["via"].toArray().isEmpty())
                    errors << "complete_transition requires an explicit via path";
            }
        }
        for (const auto& value : states) {
            const auto s = value.toObject();
            if (s.contains("fallbackState") &&
                (!names.contains(s["fallbackState"].toString()) || s["fallbackState"] == s["name"]))
                errors << "fallbackState: invalid state reference";
        }
        if (config.contains("semantics") && !config["semantics"].isObject())
            errors << "semantics: expected an object";
        if (config.contains("avatar") && !config["avatar"].isObject())
            errors << "avatar: expected an object";
        const auto avatar = config["avatar"].toObject();
        if (avatar.contains("description") && !avatar["description"].isString())
            errors << "avatar.description: expected string";
        for (const auto& field : {QString{"queueLimit"}, QString{"requestTimeoutMs"}}) {
            if (!config.contains(field))
                continue;
            const double n = config[field].toDouble(-1);
            if (!config[field].isDouble() || std::floor(n) != n || n < (field == "queueLimit" ? 0 : 1) ||
                n > (field == "queueLimit" ? 32 : 120000))
                errors << field + ": out of range or not an integer";
        }
        const auto semantics = config["semantics"].toObject();
        for (auto it = semantics.begin(); it != semantics.end(); ++it)
            if (!names.contains(it.value().toString()))
                errors << "semantics." + it.key() + ": unknown state";
        const auto policy = config["interactionPolicy"].toObject();
        const double threshold = policy["angerRefusalThreshold"].toDouble(0.85);
        if (!std::isfinite(threshold) || threshold < 0 || threshold > 1 ||
            (policy.contains("angerRefusalThreshold") && !policy["angerRefusalThreshold"].isDouble()))
            errors << "interactionPolicy.angerRefusalThreshold: expected a number in [0,1]";
        if (!errors.isEmpty())
            return errors;
        // Runtime avatar switching is intentionally not supported.
        if (m_visible || m_backend)
            return QStringList{"configuration_locked_until_restart"};
        m_config = config;
        m_current = config["defaultState"].toString();
        publish();
        return errors;
    });
}
void AvatarPresentation::setBackend(AvatarBackend* backend) {
    serial([&] {
        auto previous = m_backend;
        m_backend = nullptr;
        terminatePending("failed", "backend_unavailable");
        if (previous) {
            disconnect(previous, nullptr, this, nullptr);
            previous->setDrawing(false);
            previous->stop(m_config["defaultState"].toString());
        }
        m_backend = backend;
        if (backend) {
            Q_ASSERT(backend->thread() == thread());
            connect(backend, &AvatarBackend::cycleFinished, this, &AvatarPresentation::onCycle);
            connect(backend, &AvatarBackend::playbackFailed, this,
                    [this](quint64 token, const QString& reason) {
                        if (token != m_generation)
                            return;
                        terminatePending("failed", reason);
                        resetPlayback();
                        m_error = reason;
                        publish();
                    });
            connect(backend, &QObject::destroyed, this, [this] {
                m_backend = nullptr;
                terminatePending("failed", "backend_unavailable");
                resetPlayback();
                publish();
            });
        }
        resetPlayback();
        if (m_visible && m_backend) {
            m_backend->setDrawing(true);
            startState(m_current, &m_error);
        }
        publish();
        return true;
    });
}
void AvatarPresentation::setPolicyProvider(std::function<QJsonObject()> provider) {
    serial([&] {
        m_policy = std::move(provider);
        return true;
    });
}
QJsonObject AvatarPresentation::state(const QString& name) const {
    for (const auto& v : m_config["states"].toArray())
        if (v.toObject()["name"] == name)
            return v.toObject();
    return {};
}
QJsonObject AvatarPresentation::transition(const QString& from, const QString& to) const {
    QJsonObject wildcard;
    for (const auto& v : m_config["transactions"].toArray()) {
        const auto t = v.toObject();
        if (t["toState"] != to)
            continue;
        if (t["fromState"] == from)
            return t;
        if (t["fromState"] == "*")
            wildcard = t;
    }
    return wildcard;
}
QJsonArray AvatarPresentation::actions(const QString& name) const {
    if (!m_backend)
        return {};
    const auto resources = m_backend->resources();
    const auto durations = resources["durations"].toObject();
    auto options = state(name)["clipOptions"].toArray();
    if (options.isEmpty())
        for (const auto& id : resources["states"].toObject()[name].toArray())
            options.append(QJsonObject{{"clipName", id}});
    QJsonArray available;
    for (const auto& v : options) {
        const auto a = v.toObject();
        const auto duration = durations[a["clipName"].toString()].toDouble();
        if (std::isfinite(duration) && duration > 0)
            available.append(a);
    }
    return available;
}
QJsonObject AvatarPresentation::capabilities() const {
    return serial([&] {
        auto result = m_config;
        QJsonArray states;
        for (const auto& v : m_config["states"].toArray()) {
            auto s = v.toObject();
            s["available"] = !actions(s["name"].toString()).isEmpty();
            s["actions"] = actions(s["name"].toString());
            states.append(s);
        }
        result["states"] = states;
        result["backend_ready"] = bool(m_backend);
        return result;
    });
}
QString AvatarPresentation::interruption() const {
    const auto s = state(m_current);
    auto rule = s["interruption"].toString("immediate");
    for (const auto& v : s["clipOptions"].toArray())
        if (v.toObject()["clipName"] == m_action)
            rule = v.toObject()["interruption"].toString(rule);
    return rule;
}
QJsonObject AvatarPresentation::snapshot() const {
    return serial([&] {
        return QJsonObject{
            {"current_state", m_current},
            {"initial_state", m_config["defaultState"]},
            {"current_action", m_action},
            {"visible", m_visible},
            {"playing", m_playing},
            {"protected", bool(m_active) || (m_playing && !m_atBoundary && interruption() == "finish_cycle")},
            {"interruption", interruption()},
            {"state_version", double(m_revision)},
            {"queue_size", m_pending.size()},
            {"active_request", m_active ? m_active->id : QString{}},
            {"error", m_error}};
    });
}
QString AvatarPresentation::rejection(const Pending& p, bool checkPath) const {
    if (!m_visible)
        return "presentation_hidden";
    if (!m_backend)
        return "backend_unavailable";
    if (state(p.target).isEmpty())
        return "unknown_state";
    if (p.source == AvatarRequestSource::Model && !state(p.target)["requestable"].toBool(true))
        return "state_not_requestable";
    const auto policy = m_policy ? m_policy() : QJsonObject{};
    if (policy["forbidden_states"].toArray().contains(p.target) || state(p.target)["disabled"].toBool())
        return "state_forbidden";
    if (p.source == AvatarRequestSource::Model && policy["ai_forbidden_states"].toArray().contains(p.target))
        return "state_forbidden";
    if (p.source == AvatarRequestSource::User && policy["emotion"].toString() == "anger" &&
        policy["intensity"].toDouble() >=
            m_config["interactionPolicy"].toObject()["angerRefusalThreshold"].toDouble(0.85))
        return "emotion_declined";
    if (actions(p.target).isEmpty())
        return "action_unavailable";
    if (checkPath && p.target != m_current) {
        auto tr = transition(m_current, p.target);
        if (tr.isEmpty())
            return "transition_unavailable";
        for (const auto& via : tr["via"].toArray()) {
            if (state(via.toString())["disabled"].toBool() ||
                policy["forbidden_states"].toArray().contains(via))
                return "transition_state_forbidden";
            if (actions(via.toString()).isEmpty())
                return "transition_action_unavailable";
        }
    }
    return {};
}
QJsonObject AvatarPresentation::candidates() const {
    return serial([&] {
        QJsonArray candidates;
        for (const auto& v : m_config["states"].toArray()) {
            auto s = v.toObject();
            Pending p;
            p.target = s["name"].toString();
            p.source = AvatarRequestSource::Model;
            const auto why = rejection(p);
            if (!why.isEmpty())
                continue;
            candidates.append(QJsonObject{
                {"state", p.target},
                {"description", s["description"]},
                {"wait_required", bool(m_active) || (!m_atBoundary && interruption() == "finish_cycle")}});
        }
        return QJsonObject{{"current_state", m_current},
                           {"candidates", candidates},
                           {"reason", m_visible ? QString{} : QString{"presentation_hidden"}},
                           {"avatar", m_config["avatar"]}};
    });
}
int AvatarPresentation::priority(AvatarRequestSource s) {
    switch (s) {
    case AvatarRequestSource::User:
        return 100;
    case AvatarRequestSource::Debug:
        return 50;
    case AvatarRequestSource::Model:
        return 50;
    case AvatarRequestSource::Emotion:
        return 30;
    case AvatarRequestSource::Automatic:
        return 10;
    }
    return 0;
}
QJsonObject AvatarPresentation::report(const Pending& p, const QString& status, const QString& reason) {
    const bool terminal =
        QStringList{"completed", "cancelled", "failed", "expired", "rejected", "declined"}.contains(status);
    if (m_results.value(p.id)["terminal"].toBool())
        return m_results[p.id];
    QJsonObject result{{"request_id", p.id},
                       {"target_state", p.target},
                       {"requested_state", p.requestedTarget.isEmpty() ? p.target : p.requestedTarget},
                       {"substituted", !p.requestedTarget.isEmpty() && p.target != p.requestedTarget},
                       {"current_state", m_current},
                       {"status", status},
                       {"reason", reason},
                       {"terminal", terminal},
                       {"text_fallback_allowed", status == "rejected" || status == "failed"}};
    if (!m_results.contains(p.id))
        m_resultOrder.append(p.id);
    m_results[p.id] = result;
    while (m_resultOrder.size() > 256) {
        auto it = std::find_if(m_resultOrder.begin(), m_resultOrder.end(),
                               [&](const QString& id) { return m_results[id]["terminal"].toBool(); });
        if (it == m_resultOrder.end())
            break;
        m_results.remove(*it);
        m_resultOrder.erase(it);
    }
    emit requestChanged(result);
    return result;
}
void AvatarPresentation::finish(const Pending& p, const QString& status, const QString& reason) {
    report(p, status, reason);
}
QJsonObject AvatarPresentation::request(const QString& target, AvatarRequestSource source, int ttlMs,
                                        const QString& reason, const QString& requestId) {
    return serial([&] {
        Pending p;
        p.id = requestId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : requestId;
        p.target = target;
        p.requestedTarget = target;
        p.source = source;
        p.reason = reason;
        if (m_results.contains(p.id))
            return m_results[p.id];
        const int configured = m_config["requestTimeoutMs"].toInt(15000);
        p.deadline =
            QDateTime::currentMSecsSinceEpoch() + std::clamp(ttlMs > 0 ? ttlMs : configured, 1, 120000);
        // Fallback is opt-in per state, resolved once and reported explicitly.
        // Source permissions and configured bans apply to the original intent too.
        auto why = rejection(p, !m_active);
        if (why == "action_unavailable") {
            const auto fallback = state(p.target)["fallbackState"].toString();
            if (!fallback.isEmpty()) {
                p.target = fallback;
                why = rejection(p, !m_active);
            }
        }
        if (!why.isEmpty())
            return report(p,
                          why == "emotion_declined" ? "declined"
                                                    : (why == "backend_unavailable" ? "failed" : "rejected"),
                          why);
        if (!m_active && m_playing && p.target == m_current)
            return report(p, "completed", "already_in_state");
        const bool wait =
            m_active.has_value() || (m_playing && !m_atBoundary && interruption() == "finish_cycle");
        if (wait) {
            if (m_pending.size() >= std::clamp(m_config["queueLimit"].toInt(8), 0, 32))
                return report(p, "rejected", "queue_full");
            m_pending.append(p);
            std::stable_sort(m_pending.begin(), m_pending.end(), [](const Pending& a, const Pending& b) {
                return priority(a.source) > priority(b.source);
            });
            report(p, "queued", "protected_playback");
        } else {
            report(p, "accepted");
            begin(p);
        }
        if (m_active || !m_pending.isEmpty())
            m_expiryTimer.start();
        return m_results[p.id];
    });
}
QJsonObject AvatarPresentation::requestSemantic(const QString& semantic, AvatarRequestSource source,
                                                int ttlMs) {
    return serial([&] {
        return request(m_config["semantics"].toObject()[semantic].toString(), source, ttlMs, semantic);
    });
}
QJsonObject AvatarPresentation::interact(const QString& kind, const QString& region) {
    return serial([&] {
        emit interactionOccurred(kind, region);
        return requestSemantic(region.isEmpty() ? kind : kind + "." + region, AvatarRequestSource::User);
    });
}
bool AvatarPresentation::hasSemanticState(const QString& semantic) const {
    return serial([&] {
        const auto s = m_config["semantics"].toObject()[semantic].toString();
        return !s.isEmpty() && s == m_current;
    });
}
bool AvatarPresentation::startState(const QString& name, QString* error) {
    if (!m_visible || !m_backend) {
        *error = "backend_unavailable";
        return false;
    }
    const auto options = actions(name);
    if (options.isEmpty()) {
        *error = "action_unavailable";
        return false;
    }
    double total = 0;
    for (const auto& v : options)
        total += std::max(0.0, v.toObject()["weight"].toDouble(1));
    double sample = QRandomGenerator::global()->generateDouble() * total;
    auto option = options.last().toObject();
    for (const auto& v : options) {
        sample -= std::max(0.0, v.toObject()["weight"].toDouble(1));
        if (sample <= 0) {
            option = v.toObject();
            break;
        }
    }
    const auto action = option["clipName"].toString();
    const auto token = m_generation + 1;
    if (!m_backend->start(name, action, state(name)["loop"].toBool(true), token, error))
        return false;
    m_generation = token;
    m_current = name;
    m_action = action;
    m_playing = true;
    m_atBoundary = false;
    m_cycle = 0;
    m_error.clear();
    // This is an executor watchdog, separate from caller intent expiry. The
    // generous multiplier also tolerates slow configured animation playback.
    const double duration = m_backend->resources()["durations"].toObject()[action].toDouble();
    const double rate = m_backend->resources()["playbackRate"].toDouble(1);
    m_playbackDeadline =
        QDateTime::currentMSecsSinceEpoch() +
        static_cast<qint64>(duration * 2000 / ((std::isfinite(rate) && rate > 0) ? rate : 1)) + 5000;
    publish();
    return true;
}
void AvatarPresentation::begin(Pending p) {
    const auto why = rejection(p);
    if (!why.isEmpty()) {
        finish(p, why == "emotion_declined" ? "declined" : "rejected", why);
        return;
    }
    for (const auto& via : transition(m_current, p.target)["via"].toArray())
        p.path.append(via.toString());
    p.path.append(p.target);
    m_active = p;
    advance();
}
void AvatarPresentation::advance() {
    if (!m_active)
        return;
    // Once a protected path is entered, its finite intermediate stages must
    // finish even if willingness/policy changes. Recheck before the target.
    const auto why =
        (m_active->cancelPending || m_active->path.size() > 1) ? QString{} : rejection(*m_active, false);
    if (!why.isEmpty()) {
        const auto pending = *m_active;
        m_active.reset();
        finish(pending, "failed", why);
        drain();
        return;
    }
    const QString id = m_active->id;
    const QString next = m_active->path.takeFirst();
    QString error;
    if (!startState(next, &error)) {
        if (!m_active || m_active->id != id)
            return;
        const auto pending = *m_active;
        m_active.reset();
        m_error = error;
        finish(pending, "failed", error);
        publish();
        drain();
        return;
    }
    // Signals are synchronous. A consumer may hide the avatar in response to
    // either stateChanged or started, cancelling and clearing this request.
    if (!m_visible || !m_active || m_active->id != id)
        return;
    report(*m_active, "started");
    if (!m_visible || !m_active || m_active->id != id)
        return;
    if (m_active->path.isEmpty()) {
        const auto pending = *m_active;
        m_active.reset();
        finish(pending, "completed");
        drain();
    }
}

void AvatarPresentation::onCycle(quint64 token, quint64 cycle) {
    if (!m_visible || !m_playing || token != m_generation || cycle <= m_cycle)
        return;
    m_cycle = cycle;
    m_atBoundary = true;
    emit cycleCompleted(m_current, cycle);
    expire();
    if (token != m_generation)
        return;
    if (m_active) {
        if (m_active->cancelPending && m_active->path.size() == 1) {
            const auto p = *m_active;
            m_active.reset();
            resetPlayback();
            if (m_backend && m_visible)
                startState(m_current, &m_error);
            finish(p, "cancelled", "cancel_requested");
            drain();
        } else
            advance();
    } else {
        drain();
        if (token == m_generation && !state(m_current)["loop"].toBool(true)) {
            QString automaticTarget;
            for (const auto& v : m_config["transactions"].toArray()) {
                const auto t = v.toObject();
                if (!t["condition"].toString().isEmpty())
                    continue;
                if (t["fromState"] == m_current) {
                    automaticTarget = t["toState"].toString();
                    break;
                }
                // A concrete source edge also shadows a wildcard to the same
                // target when the concrete edge requires an explicit request.
                if (t["fromState"] == "*" && automaticTarget.isEmpty() &&
                    transition(m_current, t["toState"].toString())["fromState"] == "*")
                    automaticTarget = t["toState"].toString();
            }
            if (!automaticTarget.isEmpty())
                request(automaticTarget, AvatarRequestSource::Automatic);
        }
    }
    if (token == m_generation && state(m_current)["loop"].toBool(true))
        m_atBoundary = false;
}
void AvatarPresentation::drain() {
    if (m_active || !m_visible)
        return;
    if (!m_pending.isEmpty() && (m_atBoundary || !m_playing || interruption() != "finish_cycle")) {
        auto p = m_pending.takeFirst();
        if (p.deadline <= QDateTime::currentMSecsSinceEpoch()) {
            finish(p, "expired", "request_expired");
            drain();
        } else
            begin(p);
    }
    if (!m_active && m_pending.isEmpty())
        m_expiryTimer.stop();
}
void AvatarPresentation::expire() {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    QList<Pending> expired;
    for (int i = m_pending.size() - 1; i >= 0; --i)
        if (m_pending[i].deadline <= now)
            expired.append(m_pending.takeAt(i));
    // Remove first, then notify. An observer may hide and clear the remaining
    // queue synchronously when it receives an expiration event.
    for (const auto& pending : expired)
        finish(pending, "expired", "request_expired");
    if (m_active && m_active->deadline <= now && !m_active->cancelPending) {
        m_active->cancelPending = true;
        finish(*m_active, "expired", "request_expired");
    }
    if (m_active && m_playbackDeadline > 0 && now >= m_playbackDeadline) {
        const auto p = *m_active;
        m_active.reset();
        resetPlayback();
        m_error = "backend_boundary_timeout";
        finish(p, "failed", m_error);
        publish();
    }
    drain();
}
QJsonObject AvatarPresentation::requestResult(const QString& id) const {
    return serial([&] {
        return m_results.value(id, QJsonObject{{"status", "unknown"}, {"reason", "request_not_found"}});
    });
}
QJsonObject AvatarPresentation::cancel(const QString& id) {
    return serial([&] {
        if (m_active && m_active->id == id) {
            m_active->cancelPending = true;
            return report(*m_active, "cancel_pending", "protected_transition");
        }
        for (int i = 0; i < m_pending.size(); ++i)
            if (m_pending[i].id == id) {
                const auto p = m_pending.takeAt(i);
                finish(p, "cancelled", "cancel_requested");
                return m_results[id];
            }
        return requestResult(id);
    });
}
void AvatarPresentation::terminatePending(const QString& status, const QString& reason) {
    const auto pending = m_pending;
    m_pending.clear();
    const auto active = m_active;
    m_active.reset();
    m_expiryTimer.stop();
    if (active)
        finish(*active, status, reason);
    for (const auto& p : pending)
        finish(p, status, reason);
}
void AvatarPresentation::resetPlayback() {
    ++m_generation;
    m_cycle = 0;
    m_current = m_config["defaultState"].toString();
    m_action.clear();
    m_playing = false;
    m_atBoundary = false;
    m_playbackDeadline = 0;
    if (m_backend)
        m_backend->stop(m_current);
}
void AvatarPresentation::setVisible(bool visible) {
    serial([&] {
        if (m_visible == visible)
            return true;
        m_visible = visible;
        if (!visible) {
            if (m_backend)
                m_backend->setDrawing(false);
            resetPlayback();
            terminatePending("cancelled", "presentation_hidden");
        } else {
            resetPlayback();
            if (m_backend) {
                m_backend->setDrawing(true);
                startState(m_current, &m_error);
            }
        }
        publish();
        if (m_visible == visible)
            emit visibilityChanged(visible);
        return true;
    });
}
QJsonObject AvatarPresentation::moveTo(const QPoint& position) {
    return serial([&] {
        QPoint actual;
        const bool ok = m_backend && m_backend->moveTo(position, &actual);
        return QJsonObject{{"success", ok},
                           {"x", actual.x()},
                           {"y", actual.y()},
                           {"reason", ok ? QString{} : QString{"movement_unavailable"}}};
    });
}
void AvatarPresentation::publish() {
    ++m_revision;
    emit stateChanged(snapshot());
}
