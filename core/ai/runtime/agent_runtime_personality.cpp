#include "agent_runtime_services.h"
#include "ai/ai_brain.h"
#include "ai/context/context_assembler.h"
#include "ai/identity/sqlite_identity_repository.h"
#include "ai/identity/persona_projector.h"
#include "ai/event/event_ledger.h"
#include "ai/reflection/personality_consolidation_stage.h"
#include "ai/reflection/sleep_cycle_coordinator.h"
#include "ai/reflection/diary_service.h"
#include "ai/reflection/inner_thought_service.h"
#include "ai/scheduler/agent_scheduler.h"
#include <QSqlDatabase>
#include <QSqlQuery>

QJsonObject AgentRuntimeServices::taskIdentityContext(const QString& petName) const {
    PersonalitySnapshot personality;
    personality.baseline = m_identityBaseline;
    if (m_identityRepository) {
        const auto current = m_identityRepository->currentPersonality(m_profileId);
        if (current.isOk() && current.value()) personality = *current.value();
    }
    auto base = personality.baseline;
    base.traits = personality.effectiveTraits();
    const auto projection = PersonaProjector().projectBaseline(base, petName);
    QJsonObject persona{{"version", personality.version},
        {"traits", numericMapToJson(personality.effectiveTraits())},
        {"style", personality.behaviorStyleSummary.isEmpty()
            ? projection.promptSlots.value("speaking_style") + projection.promptSlots.value("persona_traits")
            : personality.behaviorStyleSummary},
        {"self_narrative", personality.selfNarrativeSummary}, {"pet_name", petName}};
    QJsonObject relation = numericMapToJson(relationshipDefaults());
    if (m_identityRepository) {
        const auto current = m_identityRepository->currentRelationship(m_profileId, "owner");
        if (current.isOk() && current.value()) {
            relation = numericMapToJson(current.value()->tendencies);
            relation.insert("version", current.value()->version);
        }
    }
    QJsonObject emotion;
    if (m_emotionStateProvider) {
        const auto current = m_emotionStateProvider->currentSnapshot(m_profileId, QDateTime::currentDateTimeUtc());
        emotion = {{"valence", current.value.moodValence}, {"arousal", current.value.moodArousal},
            {"intensity", current.value.intensity}, {"emotion", int(current.value.active)},
            {"observed_at", current.value.updatedAt.toString(Qt::ISODate)}};
    }
    return {{"personality", persona}, {"relationship", relation}, {"emotion", emotion}};
}

void AgentRuntimeServices::initializePersonalityStage(const SleepPolicy& policy) {
    if (!m_personalityService || !m_identityRepository || !m_aiBrain) return;
    m_personalityContextAssembler = std::make_unique<ContextAssembler>();
    m_personalityCancellation = std::make_unique<CancellationSource>();
    PersonalityEvolutionHooks hooks;
    hooks.readCommittedEvents = [this](qint64 cutoff) {
        QList<EventRecord> material;
        const auto auth = authorizationFor("reflection");
        if (!auth.isOk()) return Result<QList<EventRecord>, DomainError>::failure(auth.error());
        // Bound both SQLite work and retained envelopes on the GUI thread.
        // Evidence persists across daily windows, so its original 14-day span
        // does not depend on rereading the entire ledger on every sleep cycle.
        qint64 after = qMax<qint64>(0, cutoff - 2048);
        for (int page = 0; page < 8 && after < cutoff; ++page) {
            const auto batch = m_eventLedger->readAfter(after, {{
                QStringLiteral("UserMessageReceived"), QStringLiteral("AssistantResponseProduced"),
                QStringLiteral("UserFeedbackReceived"), QStringLiteral("TaskExperienceRecorded"),
                QStringLiteral("DiaryStored"), QStringLiteral("InnerThoughtStored")}, {}, auth.value()}, 256);
            if (!batch.isOk()) return Result<QList<EventRecord>, DomainError>::failure(batch.error());
            if (batch.value().isEmpty()) break;
            for (const auto& event : batch.value()) {
                after = event.sequence;
                if (event.sequence > cutoff) break;
                material.append(event);
            }
        }
        return Result<QList<EventRecord>, DomainError>::success(std::move(material));
    };
    hooks.readDiary = [this](const QString& id) {
        return m_diaryService ? m_diaryService->readForSelf(id)
            : Result<DiaryEntry, DomainError>::failure(domainError("PRIVATE_STORE_UNAVAILABLE", "Private diary is unavailable"));
    };
    hooks.readInnerThought = [this](const QString& id) {
        return m_innerThoughtService ? m_innerThoughtService->readSummary(id)
            : Result<InnerThoughtSummary, DomainError>::failure(domainError("PRIVATE_STORE_UNAVAILABLE", "Private reflection is unavailable"));
    };
    const auto readEvents = hooks.readCommittedEvents;
    m_personalityStage = std::make_unique<PersonalityConsolidationStage>(m_profileId,
        m_personalityService.get(), m_identityRepository.get(), m_aiBrain->modelRouter(),
        m_personalityContextAssembler.get(), std::move(hooks));
    if (m_sleepCycleCoordinator) {
        m_sleepCycleCoordinator->setPersonalityStage(m_personalityStage.get());
        m_sleepCycleCoordinator->setDiarySourceReader([readEvents](qint64 cutoff, const QDate& date) {
            const auto events = readEvents(cutoff);
            if (!events.isOk()) return Result<QList<EventRecord>, DomainError>::failure(events.error());
            QList<EventRecord> sources;
            for (const auto& event : events.value()) {
                if (event.occurredAt.toLocalTime().date() == date
                    && ((event.type == QStringLiteral("UserMessageReceived")
                            && !event.payload.value(QStringLiteral("task_related")).toBool())
                        || event.type == QStringLiteral("UserFeedbackReceived")
                        || event.type == QStringLiteral("TaskExperienceRecorded")))
                    sources.append(event);
            }
            return Result<QList<EventRecord>, DomainError>::success(sources.mid(qMax(0, int(sources.size()) - 64)));
        });
        return;
    }
    // Crypto dependencies may be absent on Windows. Public experiences can still
    // contribute during sleep; private material remains pending and encrypted.
    m_personalityTimer.setInterval(qMax(5, policy.tickIntervalSeconds) * 1000);
    const auto retryAfter = std::make_shared<QDateTime>();
    QObject::connect(&m_personalityTimer, &QTimer::timeout, &m_personalityTimer, [this, policy, retryAfter]() {
        const auto now = QDateTime::currentDateTime();
        if (!m_started || !policy.enabled || !m_aiBrain) return;
        const bool interrupted = !m_aiBrain->isEnabled() || m_aiBrain->isBusy() || m_aiBrain->isTaskActive()
            || m_aiBrain->pendingUserMessageCount() > 0
            || m_aiBrain->userIdleSeconds() < policy.minimumIdleSeconds
            || (m_agentScheduler && m_agentScheduler->hasTaskDueBefore(now.addSecs(policy.dueSoonThresholdSeconds)));
        if (m_evolutionPending && interrupted) {
            m_personalityCancellation->cancel();
            m_evolutionPending = false;
            *retryAfter = now.addSecs(policy.retryBackoffSeconds);
        }
        if (interrupted || m_evolutionPending || m_lastEvolutionDate == now.date()
            || (retryAfter->isValid() && now < *retryAfter)
            || (policy.timeBasedTrigger && now.time() < policy.bedtime)) return;
        qint64 cutoff = 0;
        const auto auth = authorizationFor("reflection");
        if (!auth.isOk()) return;
        // Ledger API is forward-only; use a separate short read connection for MAX.
        const QString connection = QStringLiteral("personality-cutoff-") + m_profileId;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(m_runtimeDatabasePath);
            if (db.open()) {
                QSqlQuery query(db);
                query.prepare("SELECT COALESCE(MAX(sequence),0) FROM event_log WHERE profile_id=?");
                query.addBindValue(m_profileId);
                if (query.exec() && query.next()) cutoff = query.value(0).toLongLong();
            }
        }
        QSqlDatabase::removeDatabase(connection);
        if (cutoff <= 0) return;
        m_evolutionPending = true;
        m_personalityCancellation->reset();
        const auto token = m_personalityCancellation->token();
        m_personalityStage->consolidateAsync({m_profileId, "sleep-personality", cutoff, now.toUTC()}, token,
            [this, token, now, policy, retryAfter](Result<PersonalitySnapshot, DomainError> result) {
                if (token.isCancelled()) return;
                m_evolutionPending = false;
                if (result.isOk()) m_lastEvolutionDate = now.date();
                else *retryAfter = QDateTime::currentDateTime().addSecs(policy.retryBackoffSeconds);
            });
    });
    if (policy.enabled) m_personalityTimer.start();
}
