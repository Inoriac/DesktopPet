#include "sleep_cycle_coordinator.h"

#include <QUuid>

#include <utility>

#include "ai/ai_brain.h"
#include "ai/scheduler/agent_scheduler.h"
#include "daydream_sleep_adapter.h"
#include "diary_service.h"
#include "sleep_session_repository.h"
#include "sqlite_private_psyche_repository.h"

namespace {

DomainError sleepError(const QString& code, const QString& message) {
    return domainError(code, message);
}

} // namespace

SleepCycleCoordinator::SleepCycleCoordinator(
    QString profileId,
    SleepPolicy policy,
    SleepSessionRepository* sessions,
    DaydreamSleepAdapter* daydream,
    DiaryService* diary,
    SqlitePrivatePsycheRepository* privateRepository,
    AIBrain* aiBrain,
    AgentScheduler* scheduler,
    SleepCycleHooks hooks)
    : m_profileId(std::move(profileId)),
      m_policy(std::move(policy)),
      m_sessions(sessions),
      m_daydream(daydream),
      m_diary(diary),
      m_privateRepository(privateRepository),
      m_aiBrain(aiBrain),
      m_scheduler(scheduler),
      m_hooks(std::move(hooks)) {
    m_timer.setSingleShot(false);
    m_timer.setInterval(qMax(5, m_policy.tickIntervalSeconds) * 1000);
    QObject::connect(&m_timer, &QTimer::timeout, [this]() { onTick(); });
}

SleepCycleCoordinator::~SleepCycleCoordinator() {
    stop();
    m_alive->store(false, std::memory_order_release);
}

bool SleepCycleCoordinator::isReady(const SleepTrigger& trigger) const {
    if (!m_policy.enabled || !m_sessions || trigger.profileId != m_profileId) return false;
    const QDateTime now = trigger.now.isValid()
        ? trigger.now : QDateTime::currentDateTime();
    if (m_backoffUntil.isValid() && now < m_backoffUntil) return false;
    if (trigger.type == SleepTriggerType::Bedtime
        && now.time() < m_policy.bedtime) {
        return false;
    }
    if (trigger.type == SleepTriggerType::Bedtime && m_diary) {
        const auto committed = m_diary->hasCommittedDiaryForDate(now.date());
        if (!committed.isOk() || committed.value()) return false;
    }
    const bool busy = m_hooks.isBrainBusy
        ? m_hooks.isBrainBusy() : (m_aiBrain && m_aiBrain->isBusy());
    if (busy) return false;
    const QDateTime dueBoundary = now.addSecs(m_policy.dueSoonThresholdSeconds);
    const bool taskDue = m_hooks.hasTaskDueBefore
        ? m_hooks.hasTaskDueBefore(dueBoundary)
        : (m_scheduler && m_scheduler->hasTaskDueBefore(dueBoundary));
    if (taskDue) return false;
    const int idle = trigger.observedIdleSeconds >= 0
        ? trigger.observedIdleSeconds
        : (m_hooks.userIdleSeconds ? m_hooks.userIdleSeconds()
                                   : (m_aiBrain ? m_aiBrain->userIdleSeconds() : -1));
    return trigger.type == SleepTriggerType::Manual
        || idle >= m_policy.minimumIdleSeconds;
}

Result<QString, DomainError> SleepCycleCoordinator::tryStart(
    const SleepTrigger& trigger) {
    if (!isReady(trigger)) {
        return Result<QString, DomainError>::failure(
            sleepError(QStringLiteral("SLEEP_NOT_READY"),
                       QStringLiteral("sleep trigger conditions are not satisfied")));
    }
    const auto incomplete = m_sessions->incomplete(m_profileId);
    if (!incomplete.isOk()) {
        return Result<QString, DomainError>::failure(incomplete.error());
    }
    if (!incomplete.value().isEmpty() || !m_activeSessionId.isEmpty()) {
        return Result<QString, DomainError>::failure(
            sleepError(QStringLiteral("SLEEP_NOT_READY"),
                       QStringLiteral("another sleep session is active")));
    }

    m_cancellation.reset();

    SleepSessionRecord session;
    session.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.profileId = m_profileId;
    if (m_hooks.sourceCutoffSequence) {
        session.sourceCutoffSequence = qMax<qint64>(
            0, m_hooks.sourceCutoffSequence());
    } else {
        const auto cutoff = m_sessions->latestEventSequence(m_profileId);
        if (!cutoff.isOk()) {
            return Result<QString, DomainError>::failure(cutoff.error());
        }
        session.sourceCutoffSequence = cutoff.value();
    }
    session.state = SleepSessionState::Snapshotting;
    session.decision = SleepDecision::Pending;
    session.startedAt = QDateTime::currentDateTimeUtc();
    const auto created = m_sessions->createPending(session);
    if (!created.isOk()) {
        return Result<QString, DomainError>::failure(created.error());
    }
    m_activeSessionId = session.sessionId;

    if (!m_daydream || !m_diary) {
        return Result<QString, DomainError>::success(session.sessionId);
    }
    const auto stateUpdated = m_sessions->updateState(
        session.sessionId, SleepSessionState::Consolidating);
    if (!stateUpdated.isOk()) {
        failPending(session.sessionId);
        return Result<QString, DomainError>::failure(stateUpdated.error());
    }

    const CancellationToken token = m_cancellation.token();
    StagingSession staging{session.sessionId, token.generation()};
    DaydreamRequest request;
    request.profileId = m_profileId;
    request.sessionId = session.sessionId;
    request.sourceCutoffSequence = session.sourceCutoffSequence;
    request.maxItems = m_policy.maxItemsPerSession;
    const std::shared_ptr<std::atomic_bool> alive = m_alive;
    m_daydream->consolidateAsync(
        request, staging, token,
        [this, alive, sessionId = session.sessionId, token]
        (Result<DaydreamChangeSet, DomainError> result) mutable {
            if (!alive->load(std::memory_order_acquire)) return;
            continueAfterDaydream(sessionId, token, std::move(result));
        });
    return Result<QString, DomainError>::success(session.sessionId);
}

void SleepCycleCoordinator::continueAfterDaydream(
    const QString& sessionId,
    const CancellationToken& token,
    Result<DaydreamChangeSet, DomainError> result) {
    if (token.isCancelled() || sessionId != m_activeSessionId) return;
    if (!result.isOk()) {
        failPending(sessionId);
        return;
    }
    const auto stateUpdated = m_sessions->updateState(
        sessionId, SleepSessionState::Journaling);
    if (!stateUpdated.isOk()) {
        failPending(sessionId);
        return;
    }
    const auto session = m_sessions->find(sessionId);
    if (!session.isOk() || !session.value().has_value()) {
        failPending(sessionId);
        return;
    }

    DiaryRequest request;
    request.profileId = m_profileId;
    request.sessionId = sessionId;
    request.localDate = QDateTime::currentDateTime().date();
    request.sourceCutoffSequence = session.value()->sourceCutoffSequence;
    StagingSession staging{sessionId, token.generation()};
    const std::shared_ptr<std::atomic_bool> alive = m_alive;
    m_diary->composeAsync(
        request, staging, token,
        [this, alive, sessionId, token]
        (Result<QString, DomainError> diaryResult) mutable {
            if (!alive->load(std::memory_order_acquire)) return;
            continueAfterDiary(sessionId, token, std::move(diaryResult));
        });
}

void SleepCycleCoordinator::continueAfterDiary(
    const QString& sessionId,
    const CancellationToken& token,
    Result<QString, DomainError> result) {
    if (token.isCancelled() || sessionId != m_activeSessionId) return;
    if (!result.isOk()) {
        failPending(sessionId);
        return;
    }
    const auto committed = m_sessions->decideCommit(sessionId);
    if (!committed.isOk()) {
        failPending(sessionId);
        return;
    }
    const auto finalized = finalizeCommitted(sessionId);
    if (!finalized.isOk()) {
        m_backoffUntil = QDateTime::currentDateTime().addSecs(
            m_policy.retryBackoffSeconds);
    }
}

Result<void, DomainError> SleepCycleCoordinator::finalizeCommitted(
    const QString& sessionId) {
    const auto session = m_sessions->find(sessionId);
    if (!session.isOk()) return Result<void, DomainError>::failure(session.error());
    if (!session.value().has_value()
        || session.value()->decision != SleepDecision::Commit) {
        return Result<void, DomainError>::failure(
            sleepError(QStringLiteral("STATE_VERSION_CONFLICT"),
                       QStringLiteral("sleep session has no Commit decision")));
    }

    if (!session.value()->finalizedParticipants.contains(QStringLiteral("memory"))) {
        if (!m_daydream) {
            return Result<void, DomainError>::failure(
                sleepError(QStringLiteral("MEMORY_STORE_UNAVAILABLE"),
                           QStringLiteral("Daydream finalize dependency is unavailable")));
        }
        const auto memory = m_daydream->finalizeSession(sessionId);
        if (!memory.isOk()) return memory;
        const auto marked = m_sessions->markParticipantFinalized(
            sessionId, QStringLiteral("memory"));
        if (!marked.isOk()) return marked;
    }

    const auto refreshed = m_sessions->find(sessionId);
    if (!refreshed.isOk()) return Result<void, DomainError>::failure(refreshed.error());
    if (!refreshed.value().has_value()) {
        return Result<void, DomainError>::failure(
            sleepError(QStringLiteral("STATE_VERSION_CONFLICT"),
                       QStringLiteral("sleep session disappeared during finalize")));
    }
    if (!refreshed.value()->finalizedParticipants.contains(
            QStringLiteral("private_psyche"))) {
        if (!m_diary) {
            return Result<void, DomainError>::failure(
                sleepError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                           QStringLiteral("Diary finalize dependency is unavailable")));
        }
        const auto diary = m_diary->finalizeSession(sessionId);
        if (!diary.isOk()) return diary;
        const auto marked = m_sessions->markParticipantFinalized(
            sessionId, QStringLiteral("private_psyche"));
        if (!marked.isOk()) return marked;
    }

    const auto completed = m_sessions->markCompleted(sessionId);
    if (!completed.isOk()) return completed;
    if (m_activeSessionId == sessionId) m_activeSessionId.clear();
    if (m_hooks.playSleepAnimation) m_hooks.playSleepAnimation();
    return Result<void, DomainError>::success();
}

Result<void, DomainError> SleepCycleCoordinator::abortPending(
    const QString& sessionId) {
    const auto session = m_sessions->find(sessionId);
    if (!session.isOk()) return Result<void, DomainError>::failure(session.error());
    if (!session.value().has_value()) return Result<void, DomainError>::success();
    if (session.value()->decision == SleepDecision::Commit) {
        return finalizeCommitted(sessionId);
    }
    if (session.value()->decision == SleepDecision::Pending) {
        const auto decided = m_sessions->decideAbort(sessionId);
        if (!decided.isOk()) return decided;
    }
    if (m_daydream) {
        const auto memory = m_daydream->abortSession(sessionId);
        if (!memory.isOk()) return memory;
    }
    Result<void, DomainError> privateAbort = Result<void, DomainError>::success();
    if (m_diary) {
        privateAbort = m_diary->abortSession(sessionId);
    } else if (m_privateRepository) {
        privateAbort = m_privateRepository->abortSession(sessionId);
    }
    if (!privateAbort.isOk()) return privateAbort;
    const auto rolledBack = m_sessions->markRolledBack(sessionId);
    if (rolledBack.isOk() && m_activeSessionId == sessionId) {
        m_activeSessionId.clear();
    }
    return rolledBack;
}

Result<void, DomainError> SleepCycleCoordinator::cancel(
    const QString& sessionId,
    SleepCancelReason reason) {
    Q_UNUSED(reason)
    if (!m_sessions) {
        return Result<void, DomainError>::failure(
            sleepError(QStringLiteral("SLEEP_CANCELLED"),
                       QStringLiteral("sleep coordinator is unavailable")));
    }
    const auto session = m_sessions->find(sessionId);
    if (!session.isOk()) return Result<void, DomainError>::failure(session.error());
    if (!session.value().has_value()) return Result<void, DomainError>::success();
    if (session.value()->decision == SleepDecision::Commit) {
        return finalizeCommitted(sessionId);
    }
    m_cancellation.cancel();
    const auto aborted = abortPending(sessionId);
    if (aborted.isOk()) {
        m_backoffUntil = QDateTime::currentDateTime().addSecs(
            m_policy.retryBackoffSeconds);
    }
    return aborted;
}

Result<void, DomainError> SleepCycleCoordinator::cancelActive(
    SleepCancelReason reason) {
    if (m_activeSessionId.isEmpty()) {
        return Result<void, DomainError>::success();
    }
    return cancel(m_activeSessionId, reason);
}

void SleepCycleCoordinator::failPending(const QString& sessionId) {
    abortPending(sessionId);
    m_backoffUntil = QDateTime::currentDateTime().addSecs(
        m_policy.retryBackoffSeconds);
}

Result<void, DomainError> SleepCycleCoordinator::recoverIncomplete() {
    if (!m_sessions) {
        return Result<void, DomainError>::failure(
            sleepError(QStringLiteral("EVENT_OUTBOX_UNAVAILABLE"),
                       QStringLiteral("sleep session repository is unavailable")));
    }
    const auto sessions = m_sessions->incomplete(m_profileId);
    if (!sessions.isOk()) return Result<void, DomainError>::failure(sessions.error());
    for (const SleepSessionRecord& session : sessions.value()) {
        const auto recovered = session.decision == SleepDecision::Commit
            ? finalizeCommitted(session.sessionId)
            : abortPending(session.sessionId);
        if (!recovered.isOk()) return recovered;
    }
    return Result<void, DomainError>::success();
}

void SleepCycleCoordinator::start() {
    if (m_started) return;
    m_cancellation.reset();
    const auto recovered = recoverIncomplete();
    if (!recovered.isOk()) {
        if (m_hooks.publishCapability) m_hooks.publishCapability(false);
        return;
    }
    m_started = true;
    if (m_aiBrain) m_aiBrain->setExternalSleepCoordinatorEnabled(true);
    if (m_hooks.publishCapability) m_hooks.publishCapability(true);
    if (m_policy.enabled) m_timer.start();
}

void SleepCycleCoordinator::stop() {
    if (!m_started && !m_timer.isActive()) {
        m_cancellation.cancel();
        return;
    }
    m_timer.stop();
    m_cancellation.cancel();
    if (!m_activeSessionId.isEmpty() && m_sessions) {
        const auto active = m_sessions->find(m_activeSessionId);
        if (active.isOk() && active.value().has_value()) {
            if (active.value()->decision == SleepDecision::Commit) {
                finalizeCommitted(m_activeSessionId);
            } else {
                abortPending(m_activeSessionId);
            }
        }
    }
    if (m_aiBrain) m_aiBrain->setExternalSleepCoordinatorEnabled(false);
    if (m_hooks.publishCapability) m_hooks.publishCapability(false);
    m_started = false;
    m_activeSessionId.clear();
}

void SleepCycleCoordinator::onTick() {
    if (!m_started) return;
    const int idle = m_hooks.userIdleSeconds
        ? m_hooks.userIdleSeconds()
        : (m_aiBrain ? m_aiBrain->userIdleSeconds() : -1);
    if (!m_activeSessionId.isEmpty()) {
        const QDateTime due = QDateTime::currentDateTime().addSecs(
            m_policy.dueSoonThresholdSeconds);
        const bool busy = m_hooks.isBrainBusy
            ? m_hooks.isBrainBusy() : (m_aiBrain && m_aiBrain->isBusy());
        const bool taskDue = m_hooks.hasTaskDueBefore
            ? m_hooks.hasTaskDueBefore(due)
            : (m_scheduler && m_scheduler->hasTaskDueBefore(due));
        if (busy || taskDue || idle < m_policy.minimumIdleSeconds) {
            cancel(m_activeSessionId, busy ? SleepCancelReason::UserInteraction
                                          : SleepCancelReason::TaskDueSoon);
        }
        return;
    }
    tryStart({SleepTriggerType::Bedtime, idle,
              QDateTime::currentDateTime(), m_profileId});
}
