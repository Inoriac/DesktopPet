#pragma once
#include "opportunity_store.h"
#include <QObject>
#include <QTimer>

class OpportunityCoordinator final : public QObject {
  public:
    OpportunityCoordinator(QString profile, QString path, QObject *parent = nullptr);
    OpportunityStore store;
    AutonomyConfig config;
    OpportunitySignal current;
    std::function<void(OpportunityTicket)> consumer;
    std::function<void(IdleDecision)> publishDecision;
    std::function<void()> requestReadiness;
    Result<void, DomainError> configure(const AutonomyConfig &);
    Result<void, DomainError> publish(const OpportunitySignal &);
    void consider(OpportunitySignal);

  private:
    QTimer m_timer;
    QString m_hostEpoch;
    DomainError m_initializationError;
};
