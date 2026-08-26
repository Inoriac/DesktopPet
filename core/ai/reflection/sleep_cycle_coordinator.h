#ifndef DESKTOP_PET_SLEEP_CYCLE_COORDINATOR_H
#define DESKTOP_PET_SLEEP_CYCLE_COORDINATOR_H

#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>

#include "ai/memory/daydream_consolidator.h"
#include "cancellation_token.h"
#include "reflection_types.h"

class AIBrain;
class AgentScheduler;
class DaydreamSleepAdapter;
class DiaryService;
class SleepSessionRepository;
class SqlitePrivatePsycheRepository;

struct SleepCycleHooks {
    std::function<bool()> isBrainBusy;
    std::function<bool(const QDateTime&)> hasTaskDueBefore;
    std::function<int()> userIdleSeconds;
    std::function<qint64()> sourceCutoffSequence;
    std::function<void(bool)> publishCapability;
    std::function<void()> playSleepAnimation;
};

class SleepCycleCoordinator {
public:
    SleepCycleCoordinator(
        QString profileId,
        SleepPolicy policy,
        SleepSessionRepository* sessions,
        DaydreamSleepAdapter* daydream,
        DiaryService* diary,
        SqlitePrivatePsycheRepository* privateRepository,
        AIBrain* aiBrain,
        AgentScheduler* scheduler,
        SleepCycleHooks hooks);
    ~SleepCycleCoordinator();

    Result<void, DomainError> recoverIncomplete();
    void start();
    void stop();
    Result<QString, DomainError> tryStart(const SleepTrigger& trigger);
    Result<void, DomainError> cancel(const QString& sessionId,
                                     SleepCancelReason reason);
    Result<void, DomainError> cancelActive(SleepCancelReason reason);

    quint64 generation() const { return m_cancellation.generation(); }
    bool isStarted() const { return m_started; }

private:
    bool isReady(const SleepTrigger& trigger) const;
    void continueAfterDaydream(
        const QString& sessionId,
        const CancellationToken& token,
        Result<DaydreamChangeSet, DomainError> result);
    void continueAfterDiary(
        const QString& sessionId,
        const CancellationToken& token,
        Result<QString, DomainError> result);
    Result<void, DomainError> finalizeCommitted(const QString& sessionId);
    Result<void, DomainError> abortPending(const QString& sessionId);
    void failPending(const QString& sessionId);
    void onTick();

    QString m_profileId;
    SleepPolicy m_policy;
    SleepSessionRepository* m_sessions = nullptr;
    DaydreamSleepAdapter* m_daydream = nullptr;
    DiaryService* m_diary = nullptr;
    SqlitePrivatePsycheRepository* m_privateRepository = nullptr;
    AIBrain* m_aiBrain = nullptr;
    AgentScheduler* m_scheduler = nullptr;
    SleepCycleHooks m_hooks;
    QTimer m_timer;
    CancellationSource m_cancellation;
    QString m_activeSessionId;
    QDateTime m_backoffUntil;
    bool m_started = false;
    std::shared_ptr<std::atomic_bool> m_alive =
        std::make_shared<std::atomic_bool>(true);
};

#endif // DESKTOP_PET_SLEEP_CYCLE_COORDINATOR_H
