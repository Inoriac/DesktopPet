#pragma once
#include "ai/plugin/capability_types.h"
#include "ai/task/task_types.h"
#include <QDateTime>

enum class ActivityState {
    Ready,
    Running,
    WaitingExternal,
    WaitingUser,
    Paused,
    Completed,
    Cancelled,
    Failed
};
enum class ResumeActor { User, Event, Scheduler };
enum class ActionOutcome { Accepted, Succeeded, Failed, Unknown, NotExecuted };
QString activityStateName(ActivityState state);
std::optional<ActivityState> activityStateFromName(const QString &value);
bool activityTerminal(ActivityState state);
struct ActivityBudget {
    TaskBudget limits;
    int toolCalls = 0, replans = 0;
    qint64 activeMs = 0;
    QJsonObject toJson() const;
    static ActivityBudget fromJson(const QJsonObject &value);
};
struct ActivityIntent {
    QString goal, reason, origin = "user", stopWhen, reconsiderWhen;
    ContextSpec context;
    QStringList evidenceRefs;
    QList<CapabilityRef> capabilityRefs;
    TaskBudget budget;
    QJsonObject toJson() const;
};
struct ActivitySnapshot {
    QString activityId, profileId;
    ActivityState state = ActivityState::Ready;
    qint64 revision = 0;
    quint64 generation = 0;
    ContextSpec context;
    QJsonObject intent, execution, checkpoint, waitingCondition;
    QString executionPhase, pauseReason, result;
    ActivityBudget budget;
};
struct ActivityTransition {
    QString profileId, activityId;
    qint64 expectedRevision = 0;
    ActivityState targetState = ActivityState::Ready;
    QString reason, executionPhase, result;
    QJsonObject checkpoint, waitingCondition;
    QStringList evidenceRefs;
    bool emptyCheck = false;
    qint64 activeMs = -1; // Host cumulative clock sample, never model input.
};
struct ResumeActivityRequest {
    QString profileId, activityId;
    qint64 expectedRevision = 0;
    QString triggerRef;
    ResumeActor resumeActor = ResumeActor::User;
    QString expectedForegroundId;
    qint64 expectedForegroundRevision = 0;
    qint64 foregroundActiveMs = -1; // Host cumulative clock sample.
};
struct ActionPreparation {
    QString profileId, activityId;
    qint64 expectedRevision = 0;
    quint64 generation = 0;
    QString logicalStepId;
    CapabilityRef capabilityRef;
    QJsonObject arguments;
    bool readOnly = false;
};
struct ActionDispatch {
    QString profileId, activityId, callId;
    qint64 expectedRevision = 0;
    quint64 generation = 0;
};
struct ActionSnapshot {
    QString profileId, activityId, callId, logicalKey, argumentRef, status;
    quint64 generation = 0;
    CapabilityRef capabilityRef;
    QJsonObject result;
};
struct ActionObservation {
    QString profileId, activityId, callId;
    quint64 dispatchGeneration = 0;
    ActionOutcome outcome = ActionOutcome::Unknown;
    QJsonObject receipt;
    QDateTime observedAt;
};
