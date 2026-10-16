#pragma once
#include "activity_types.h"
#include "selection_types.h"

// Value object: no database connection or QObject crosses worker boundaries.
class ActivityStore {
  public:
    ActivityStore(QString profileId, QString databasePath);
    const QString &profileId() const { return m_profileId; }
    const QString &databasePath() const { return m_databasePath; }
    Result<void, DomainError> initialize();
    Result<void, DomainError> recoverAfterRestart();
    Result<qint64, DomainError> remainingAutonomousActiveMs() const;
    Result<ActivitySnapshot, DomainError> find(const QString &activityId) const;
    Result<QList<ActivitySnapshot>, DomainError>
    list(const QList<ActivityState> &states = {}, int limit = 0) const;
    Result<QList<ActivitySnapshot>, DomainError>
    selectionCandidates(int limit = 8) const;
    Result<ActivitySnapshot, DomainError>
    applySelection(const SelectionAdoption &);
    Result<void, DomainError>
    validateImportantReply(const QString &eventId) const;
    Result<QStringList, DomainError> pendingReplyEvents() const;
    Result<bool, DomainError> routeObservedReply(const QString &eventId);
    Result<QJsonObject, DomainError>
    resolveWaitCondition(const QString &activityId, const QString &callId,
                         const QString &kind) const;
    Result<ActivitySnapshot, DomainError>
    startActivity(const ActivityIntent &intent);
    Result<ActivitySnapshot, DomainError>
    transitionActivity(const ActivityTransition &transition);
    Result<ActivitySnapshot, DomainError>
    resumeActivity(const ResumeActivityRequest &request);
    Result<ActionSnapshot, DomainError>
    prepareAction(const ActionPreparation &request);
    Result<ActionSnapshot, DomainError>
    markActionDispatched(const ActionDispatch &request);
    Result<void, DomainError>
    observeAction(const ActionObservation &observation);
    Result<ActionSnapshot, DomainError> findAction(const QString &callId) const;
    Result<void, DomainError> saveTask(TaskRecord &task,
                                       bool enqueueExperience = false);
    Result<TaskRecord, DomainError> task(const QString &activityId) const;
    Result<QJsonArray, DomainError> pendingExperiences() const;
    Result<void, DomainError> acknowledgeExperience(const QString &activityId);
    using CapabilityValidator = std::function<Result<void, DomainError>(
        const QList<CapabilityRef> &, const ContextSpec &, bool, quint64)>;
    void setCapabilityValidator(CapabilityValidator validator) {
        m_capabilityValidator = std::move(validator);
    }

  private:
    Result<ActivitySnapshot, DomainError>
    startActivityImpl(const ActivityIntent &, const SelectionAdoption *);
    Result<ActivitySnapshot, DomainError>
    resumeActivityImpl(const ResumeActivityRequest &,
                       const SelectionAdoption *);
    QString m_profileId, m_databasePath;
    CapabilityValidator m_capabilityValidator;
};
