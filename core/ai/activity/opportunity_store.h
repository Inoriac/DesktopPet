#pragma once
#include "ai/scheduler/opportunity_types.h"
#include <QSqlDatabase>
#include "ai/context/context_spec.h"

// Path-only backend. Production calls run on ActivityRuntime's serial worker.
class OpportunityStore {
  public:
    OpportunityStore(QString profileId, QString databasePath);
    Result<void, DomainError> configure(const AutonomyConfig &, const OpportunityClock &);
    Result<void, DomainError> bindHostEpoch(const QString &, const OpportunityClock &);
    Result<void, DomainError> setPaused(bool, const OpportunityClock &);
    Result<void, DomainError> publishReadiness(const OpportunitySignal &);
    Result<OpportunityTicket, DomainError> considerOpportunity(const OpportunitySignal &);
    Result<void, DomainError> finishOpportunity(const OpportunityFinish &);
    Result<ModelBudgetReservation, DomainError> reserveModelAttempt(const ModelBudgetRequest &);
    Result<void, DomainError> settleModelAttempt(const ModelBudgetSettlement &);
    Result<QJsonObject, DomainError> snapshot() const;
    Result<qint64, DomainError> remainingActiveMs(const OpportunityClock &) const;
    // Called inside the activity checkpoint UoW; both ledgers commit together.
    static Result<qint64, DomainError> accountActive(QSqlDatabase, const QString &profile,
                                                     const QString &activityId, qint64 cumulativeMs,
                                                     const OpportunityClock &);
    static Result<void, DomainError>
    consumeSelection(QSqlDatabase, const QString &, const OpportunityTicket &,
                     const QString &decision, const OpportunityClock &);
    static Result<void, DomainError>
    recordActualCheck(QSqlDatabase, const QString &profile,
                      const QString &activityId, const QString &callId,
                      const QDateTime &);
    static Result<void, DomainError> validateMemoryScope(QSqlDatabase,
                                                         const QString &profile,
                                                         const ContextSpec &);
    static OpportunityClock systemClock();

  private:
    QString m_profileId, m_databasePath, m_hostEpoch;
};
