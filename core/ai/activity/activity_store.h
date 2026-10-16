#pragma once
#include "activity_types.h"

// Value object: no database connection or QObject crosses worker boundaries.
class ActivityStore {
  public:
    ActivityStore(QString profileId, QString databasePath);
    const QString &profileId() const { return m_profileId; }
    const QString &databasePath() const { return m_databasePath; }
    Result<void, DomainError> initialize();
    Result<void, DomainError> recoverAfterRestart();
    Result<ActivitySnapshot, DomainError> find(const QString &activityId) const;
    Result<QList<ActivitySnapshot>, DomainError>
    list(const QList<ActivityState> &states = {}) const;
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
        const QList<CapabilityRef> &, const ContextSpec &)>;
    void setCapabilityValidator(CapabilityValidator validator) {
        m_capabilityValidator = std::move(validator);
    }

  private:
    QString m_profileId, m_databasePath;
    CapabilityValidator m_capabilityValidator;
};
