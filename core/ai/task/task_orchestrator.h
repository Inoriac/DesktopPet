#pragma once
#include "task_store.h"
#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

class TaskOrchestrator : public QObject {
    Q_OBJECT
public:
    explicit TaskOrchestrator(TaskStore* store, QObject* parent = nullptr);
    ~TaskOrchestrator() override;
    void setDependencies(TaskDependencies dependencies);
    bool start(const TaskStartRequest& request);
    bool recover(const QString &activityId = {});
    bool activate(const QString &activityId);
    void releaseCurrent();
    qint64 currentActiveMs();
    bool reloadCurrent();
    bool resume(const QString& feedback = {}, const TaskBudgetExtension& extension = {});
    void pause(const QString& reason = QStringLiteral("用户暂停了任务。"));
    void stop(const QString& reason = QStringLiteral("用户停止了任务。"));
    void shutdown();
    bool isActive() const { return m_active; }
    bool hasUnfinishedTask() const { return !m_task.id.isEmpty() && !m_task.terminal(); }
    const TaskRecord& currentTask() const { return m_task; }
    // Signals are coalesced by trigger ID and judged only at safe checkpoints.
    void requestSafeCheckpoint(const QString &eventId);
    void notifyAgencyTrigger(const QString& eventId, const QString& reason);
    // Unknown side effects must be checked externally or explicitly confirmed by the user.
    bool resolveUnknownCall(const QString& callId, bool succeeded, const QJsonObject& observation);
    bool allowedTool(const QString& name) const;
    bool readOnlyTool(const QString& name,const QJsonObject& arguments={}) const;
signals:
    void taskChanged(const QJsonObject& task);
    void userMessage(const QString& message);
    void taskFinished(const QJsonObject& task);
    void experienceReady(const QString& taskId);
    void activeChanged(bool active);
private:
  bool applyRequestedCheckpoint();
  QString m_checkpointTrigger;
  void accountTime();
  void setActive(bool active);
  bool persist(bool terminal = false);
  bool transition(const QString &status);
  void cancelPending();
  void suspend(const QString &status, const QString &reason);
  void pauseWithReason(const QString &reason, const QString &hostReason);
  void finish(const QString &status, const QString &reason,
              const QString &result = {});
  void assess(bool checkpoint, int attempt = 0);
  void plan(bool replan = false, int attempt = 0);
  void verify(int attempt = 0);
  void waitForExternal(const QJsonObject &proposal);
  void replan(const QString &reason);
  void dispatchTool(const QJsonObject &tool);
  void dispatchValidatedTool(const QJsonObject &tool, bool readOnly);
  void afterObservation();
  void checkpointThenPlan();
  void refreshContext();
  bool hasUnknownAction() const;
  bool hasRequiredCapability() const;
  bool checkTimeBudget();
  void callModel(TaskModelRequest request,
                 std::function<void(TaskModelReply)> callback);
  QJsonObject modelContext() const;
  void heartbeat();
  TaskStore *m_store = nullptr;
  TaskDependencies m_dependencies;
  TaskRecord m_task;
  QJsonArray m_modelCapabilitySteps;
  bool m_active = false;
  bool m_pendingTool = false;
  QString m_pendingId;
  quint64 m_generation = 0;
  quint64 m_requestSequence = 0;
  QElapsedTimer m_clock;
  QTimer m_deadline;
  QTimer m_heartbeat;
};
