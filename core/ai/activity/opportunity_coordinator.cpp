#include "opportunity_coordinator.h"
#include <QUuid>

OpportunityCoordinator::OpportunityCoordinator(QString p, QString path, QObject *parent)
    : QObject(parent), store(std::move(p), std::move(path)),
      m_hostEpoch(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    auto bound = store.bindHostEpoch(m_hostEpoch, OpportunityStore::systemClock());
    if (!bound)
        m_initializationError = bound.error();
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (requestReadiness) {
            requestReadiness();
            return;
        }
        auto signal = current;
        signal.kind = "tick";
        signal.clock = OpportunityStore::systemClock();
        consider(signal);
    });
}
Result<void, DomainError> OpportunityCoordinator::configure(const AutonomyConfig &c) {
    if (!m_initializationError.code.isEmpty())
        return Result<void, DomainError>::failure(m_initializationError);
    config = c;
    auto r = store.configure(c, OpportunityStore::systemClock());
    if (r && c.enabled)
        m_timer.start(c.localTickMs);
    else
        m_timer.stop();
    return r;
}
Result<void, DomainError> OpportunityCoordinator::publish(const OpportunitySignal &input) {
    if (!m_initializationError.code.isEmpty())
        return Result<void, DomainError>::failure(m_initializationError);
    auto s = input;
    s.readiness.hostEpoch = m_hostEpoch;
    if (s.readiness.hostRevision < current.readiness.hostRevision)
        return Result<void, DomainError>::failure(
            domainError("STALE_CONTEXT", "Old host snapshot"));
    current = s;
    auto r = store.publishReadiness(s);
    if (publishDecision) {
        auto effective = s.readiness;
        if (r) {
            auto persisted = store.snapshot();
            if (!persisted)
                r = Result<void, DomainError>::failure(persisted.error());
            else {
                effective.paused = effective.paused || persisted.value()["paused"].toBool();
                effective.autonomyEnabled = persisted.value()["autonomyEnabled"].toBool();
                effective.selectionReservation =
                    persisted.value()["selectionReservation"].toObject()["ticketId"].toString();
            }
        }
        auto d = evaluateIdle(effective);
        if (!r) {
            d.canDeliberate = false;
            d.reason = r.error().code;
        }
        publishDecision(d);
    }
    return r;
}
void OpportunityCoordinator::consider(OpportunitySignal signal) {
    signal.readiness.hostEpoch = m_hostEpoch;
    if (signal.readiness.hostRevision < current.readiness.hostRevision)
        return;
    if (!publish(signal))
        return;
    if (!consumer)
        return;
    auto r = store.considerOpportunity(signal);
    if (r)
        consumer(r.value());
}
