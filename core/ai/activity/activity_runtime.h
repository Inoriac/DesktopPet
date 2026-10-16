#pragma once
#include "activity_store.h"
#include <QObject>
#include <QPointer>
#include <QThread>
class TaskStore;
class TaskOrchestrator;

// GUI facade. All storage, runner state and timers belong to one serial worker.
class ActivityRuntime final : public QObject {
    Q_OBJECT
  public:
    ActivityRuntime(QString profileId, QString databasePath,
                    QObject *parent = nullptr);
    ~ActivityRuntime() override;
    using Completion =
        std::function<void(Result<ActivitySnapshot, DomainError>)>;
    void setDependencies(TaskDependencies dependencies);
    void setCapabilityValidator(ActivityStore::CapabilityValidator validator);
    void startTaskAsync(TaskStartRequest request);
    void resumeTaskAsync(QString activityId, QString feedback = {},
                         TaskBudgetExtension extension = {});
    void pauseTaskAsync();
    void stopTaskAsync();
    void shutdownAsync();
    void resolveUnknownAsync(QString activityId, QString callId,
                             bool succeeded);
    void startActivityAsync(ActivityIntent intent, Completion completion);
    void transitionActivityAsync(ActivityTransition transition,
                                 Completion completion);
    void resumeActivityAsync(ResumeActivityRequest request,
                             Completion completion);
    void prepareActionAsync(
        ActionPreparation request,
        std::function<void(Result<ActionSnapshot, DomainError>)> completion);
    void markActionDispatchedAsync(
        ActionDispatch request,
        std::function<void(Result<ActionSnapshot, DomainError>)> completion);
    void observeActionAsync(
        ActionObservation observation,
        std::function<void(Result<void, DomainError>)> completion);
    void listActivitiesAsync(
        std::function<void(Result<QList<ActivitySnapshot>, DomainError>)>
            completion);
    void pendingExperiencesAsync(
        std::function<void(Result<QJsonArray, DomainError>)> completion);
    void acknowledgeExperienceAsync(
        QString id, std::function<void(Result<void, DomainError>)> completion);
    QJsonObject taskSnapshot() const { return m_snapshot; }
    bool isActive() const { return m_active; }
    bool hasUnfinishedTask() const;
    const QString &profileId() const { return m_profileId; }
    const QString &databasePath() const { return m_databasePath; }
  signals:
    void taskChanged(const QJsonObject &task);
    void userMessage(const QString &text);
    void taskFinished(const QJsonObject &task);
    void activeChanged(bool active);
    void experienceReady(const QString &id);
    void ready();

  private:
    struct Worker;
    Worker *m_worker = nullptr;
    QThread m_thread;
    QString m_profileId, m_databasePath;
    QJsonObject m_snapshot;
    bool m_active = false;
};
