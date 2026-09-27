#include "agent_runtime_services.h"

#include <QLockFile>
#include <QRegularExpression>

#include "ai/event/event_ledger.h"
#include "ai/identity/personality_service.h"
#include "ai/identity/relationship_service.h"
#include "ai/identity/self_model_service.h"
#include "ai/identity/sqlite_identity_repository.h"

namespace {
struct Feedback { QString trait; double direction; QString context; };

// Only explicit feedback about this companion is evidence. Ordinary user facts,
// model output, screen observations and temporary focus requests do not train traits.
std::optional<Feedback> explicitFeedback(const QString& input) {
    const QString text = input.simplified().left(512);
    if (text.contains(QRegularExpression(QStringLiteral("(?:分钟|小时|今天|暂时|现在|勿扰|专注)"))))
        return std::nullopt;
    struct Rule { const char16_t* pattern; const char* trait; double direction; const char* context; };
    static const Rule rules[] = {
        {u"^(?:以后|希望你|请你|你可以|你)?(?:少主动|不要主动|别主动|主动少一点)", "initiative", -1, "pace_request"},
        {u"^(?:以后|希望你|请你|你可以|你)?(?:多主动|主动一点|更主动)", "initiative", 1, "pace_request"},
        {u"^(?:我)?(?:很)?喜欢你主动", "initiative", 1, "pace_feedback"},
        {u"^(?:我)?不喜欢你主动", "initiative", -1, "pace_feedback"},
        {u"^(?:以后|希望你|请你|你可以|你)?(?:多陪我聊|多和我聊)", "sociability", 1, "companionship_request"},
        {u"^(?:我)?(?:很)?喜欢和你聊天", "sociability", 1, "companionship_feedback"},
        {u"^(?:谢谢你|你做得好|做得好|你很棒|干得好)[！!。 ，,]*$", "sociability", 0.3, "explicit_praise"},
        {u"^(?:以后|希望你|请你|你可以|你)?(?:多尝试新|多探索新)", "openness", 1, "exploration_request"},
        {u"^(?:我)?喜欢你尝试新", "openness", 1, "exploration_feedback"}
    };
    for (const auto& rule : rules) {
        if (QRegularExpression(QString::fromUtf16(rule.pattern)).match(text).hasMatch())
            return Feedback{QString::fromLatin1(rule.trait), rule.direction, QString::fromLatin1(rule.context)};
    }
    return std::nullopt;
}
}

Result<void, DomainError> AgentRuntimeServices::processIdentityEvents(const QDateTime& now) {
    if (!m_started || !m_identityRepository || !m_checkpointStore || !now.isValid())
        return Result<void, DomainError>::success();
    QLockFile lock(m_runtimeDatabasePath + QStringLiteral(".identity.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return Result<void, DomainError>::success();
    const QString consumer = QStringLiteral("identity_growth_v1");
    auto checkpoint = m_checkpointStore->current(consumer);
    if (!checkpoint.isOk()) return Result<void, DomainError>::failure(checkpoint.error());
    const auto authorization = authorizationFor(QStringLiteral("identity"));
    if (!authorization.isOk()) return Result<void, DomainError>::failure(authorization.error());
    const auto events = m_eventLedger->readAfter(checkpoint.value(),
        {{QStringLiteral("UserMessageReceived")}, {}, authorization.value()}, 64);
    if (!events.isOk()) return Result<void, DomainError>::failure(events.error());
    qint64 sequence = checkpoint.value();
    for (const auto& event : events.value()) {
        const QString trigger = event.payload.value(QStringLiteral("triggerTag")).toString();
        const auto feedback = (trigger == QLatin1String("manual") || trigger == QLatin1String("user_request"))
            && event.privacy == EventPrivacy::Normal
            ? explicitFeedback(event.payload.value(QStringLiteral("text")).toString()) : std::nullopt;
        if (feedback) {
            const TraitEvidenceDraft evidence{feedback->trait, feedback->direction, 0.5, 1.0,
                feedback->context, event.eventId, event.occurredAt};
            const auto stored = m_personalityService->recordEvidence(evidence);
            if (!stored.isOk()) return stored;
            const auto relationship = m_relationshipService->applyEvidence(QStringLiteral("owner"),
                {feedback->trait, feedback->direction, 0.2, 1.0, event.eventId, event.occurredAt});
            if (!relationship.isOk()) return Result<void, DomainError>::failure(relationship.error());
        }
        const auto committed = m_checkpointStore->commit(consumer, sequence, event.sequence);
        if (!committed.isOk()) return committed;
        sequence = event.sequence;
    }

    auto personality = m_identityRepository->currentPersonality(m_profileId);
    if (!personality.isOk()) return Result<void, DomainError>::failure(personality.error());
    const auto pending = m_identityRepository->pendingEvidence(m_profileId,
        QDateTime::fromSecsSinceEpoch(0, Qt::UTC), now.toUTC());
    if (!pending.isOk()) return Result<void, DomainError>::failure(pending.error());
    if (!pending.value().isEmpty()) {
        QDateTime first = now;
        for (const auto& evidence : pending.value()) if (evidence.createdAt < first) first = evidence.createdAt;
        const bool windowElapsed = first.addDays(m_personalityPolicy.minimumWindowDays) <= now;
        const bool nextWindow = !personality.value()
            || personality.value()->effectiveAt.addDays(m_personalityPolicy.minimumWindowDays) <= now;
        if (windowElapsed && nextWindow) {
            const auto consolidated = m_personalityService->consolidate({m_profileId, first, now,
                personality.value() ? int(personality.value()->version) : 0});
            if (!consolidated.isOk()) return Result<void, DomainError>::failure(consolidated.error());
            personality = m_identityRepository->currentPersonality(m_profileId);
            if (!personality.isOk()) return Result<void, DomainError>::failure(personality.error());
        }
    }
    if (!personality.value()) return Result<void, DomainError>::success();
    const auto self = m_identityRepository->currentSelfModel(m_profileId);
    if (!self.isOk()) return Result<void, DomainError>::failure(self.error());
    const auto& state = *personality.value();
    if (self.value()) {
        for (const auto& evidence : self.value()->evidence.items)
            if (evidence.sourceType == EvidenceSourceType::PersonalityVersion && evidence.referenceId == state.stateId)
                return Result<void, DomainError>::success();
    }
    QStringList tendencies;
    for (auto it = state.tendencies.cbegin(); it != state.tendencies.cend(); ++it) {
        if (qAbs(it.value()) < 1e-9) continue;
        if (it.key() == QLatin1String("initiative")) tendencies << (it.value() > 0 ? QStringLiteral("更主动地陪伴") : QStringLiteral("给主人更多安静空间"));
        if (it.key() == QLatin1String("sociability")) tendencies << (it.value() > 0 ? QStringLiteral("更愿意交流") : QStringLiteral("交流更克制"));
        if (it.key() == QLatin1String("openness")) tendencies << (it.value() > 0 ? QStringLiteral("更愿意探索新事物") : QStringLiteral("做事更稳妥"));
    }
    if (tendencies.isEmpty()) return Result<void, DomainError>::success();
    SelfModelProposal proposal;
    proposal.narrative = QStringLiteral("根据主人长期表达的相处偏好，我在学习%1；仍会尊重当下的意愿。").arg(tendencies.join(QStringLiteral("、")));
    if (self.value()) proposal.parentVersionId = self.value()->versionId;
    proposal.proposedAt = now;
    const auto evolved = m_selfModelService->evolve(proposal,
        {{{state.stateId, EvidenceSourceType::PersonalityVersion, true}}});
    if (!evolved.isOk()) return Result<void, DomainError>::failure(evolved.error());
    return Result<void, DomainError>::success();
}
