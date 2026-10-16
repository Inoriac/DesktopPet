#include "agent_runtime_services.h"

#include <QLockFile>
#include <QRegularExpression>

#include "ai/event/event_ledger.h"
#include "ai/identity/personality_service.h"
#include "ai/identity/relationship_service.h"
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

QList<QPair<QString, double>> explicitRelationshipFeedback(const QString& input) {
    const QString text = input.simplified().left(512);
    QList<QPair<QString, double>> result;
    const auto matches = [&text](const QString& expression) {
        return QRegularExpression(expression).match(text).hasMatch();
    };
    if (matches(QStringLiteral("^(?:我)?(?:很)?(?:喜欢和你聊天|喜欢你主动|喜欢你尝试新)|^(?:谢谢你|你做得好|做得好|你很棒|干得好)[！!。 ，,]*$")))
        result.append({QStringLiteral("affinity"), 1.0});
    if (matches(QStringLiteral("^(?:我(?:很)?信任你|你说得(?:对|很准确)|你的回答很可靠)[！!。 ，,]*$")))
        result.append({QStringLiteral("trust"), 1.0});
    if (matches(QStringLiteral("^(?:你说错了|你弄错了|这个答案错了|你的回答不准确)[！!。 ，,]*$")))
        result.append({QStringLiteral("trust"), -1.0});
    if (matches(QStringLiteral("^(?:你可以休息|你可以拒绝|我尊重你的边界|没关系，你先休息)[！!。 ，,]*$")))
        result.append({QStringLiteral("boundaryRespect"), 1.0});
    if (matches(QStringLiteral("^(?:你不许拒绝|你必须听我的|你没有权利拒绝)[！!。 ，,]*$")))
        result.append({QStringLiteral("boundaryRespect"), -1.0});
    return result;
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
        {{QStringLiteral("UserMessageReceived"), QStringLiteral("TaskExperienceRecorded")}, {}, authorization.value()}, 64);
    if (!events.isOk()) return Result<void, DomainError>::failure(events.error());
    qint64 sequence = checkpoint.value();
    for (const auto& event : events.value()) {
        const QString trigger = event.payload.value(QStringLiteral("triggerTag")).toString();
        const bool interaction = event.type == QLatin1String("UserMessageReceived")
            && (trigger == QLatin1String("manual") || trigger == QLatin1String("user_request"))
            && !event.payload.value(QStringLiteral("task_related")).toBool()
            && event.privacy == EventPrivacy::Normal;
        const bool taskExperience = event.type == QLatin1String("TaskExperienceRecorded")
            && event.privacy == EventPrivacy::Normal;
        const QString text = interaction ? event.payload.value(QStringLiteral("text")).toString() : QString();
        auto origin = originKindFromString(event.payload.value(QStringLiteral("origin_kind")).toString());
        if (!event.payload.contains(QStringLiteral("origin_kind"))
            && ((event.type == QLatin1String("UserMessageReceived") && event.source == QLatin1String("AIBrain"))
                || (event.type == QLatin1String("TaskExperienceRecorded") && event.source == QLatin1String("TaskExperienceAdapter"))))
            origin = OriginKind::LocalOwner;
        QString subject = event.payload.value(QStringLiteral("subject_id")).toString();
        if (subject.trimmed().isEmpty() && origin == OriginKind::LocalOwner)
            subject = QStringLiteral("owner");
        bool resolved = false;
        if (origin.has_value() && origin != OriginKind::Internal && !subject.trimmed().isEmpty()) {
            const auto person = m_identityRepository->personRevision(m_profileId, subject);
            if (!person.isOk()) return Result<void, DomainError>::failure(person.error());
            resolved = person.value().has_value();
        }
        const auto feedback = resolved && origin == OriginKind::LocalOwner
            && subject == QLatin1String("owner") && interaction ? explicitFeedback(text) : std::nullopt;
        const auto updateRelationship = [&](const QString& dimension, double direction, double weight) {
            return m_relationshipService->applyEvidence(subject,
                {dimension, direction, weight, 1.0, event.eventId, event.occurredAt});
        };
        if (feedback) {
            TraitEvidenceDraft evidence{feedback->trait, feedback->direction, 0.5, 1.0,
                feedback->context, event.eventId, event.occurredAt};
            evidence.sourceSequence = event.sequence;
            evidence.sourceCategory = QStringLiteral("explicit_feedback");
            evidence.originalSourceId = event.eventId;
            evidence.originalOccurredAt = event.occurredAt;
            const auto stored = m_personalityService->recordEvidence(evidence);
            if (!stored.isOk()) return stored;
        }
        // Shared interactions build familiarity, without interpreting silence or
        // a task's technical outcome as approval, dislike, or a trust judgment.
        const QString taskStatus = event.payload.value(QStringLiteral("status")).toString();
        if (resolved && ((interaction && text.simplified().size() >= 2)
            || (taskExperience && (taskStatus == QLatin1String("Completed")
                || taskStatus == QLatin1String("Failed") || taskStatus == QLatin1String("Cancelled"))))) {
            const auto relationship = updateRelationship(QStringLiteral("familiarity"), 1.0, taskExperience ? 0.25 : 0.10);
            if (!relationship.isOk()) return Result<void, DomainError>::failure(relationship.error());
        }
        const QString explicitText = !resolved ? QString() : interaction ? text
            : taskExperience && event.payload.value(QStringLiteral("feedback_status")).toString() == QLatin1String("provided")
                ? event.payload.value(QStringLiteral("user_feedback")).toString() : QString();
        for (const auto& [dimension, direction] : explicitRelationshipFeedback(explicitText)) {
            const auto relationship = updateRelationship(dimension, direction, 0.60);
            if (!relationship.isOk()) return Result<void, DomainError>::failure(relationship.error());
        }
        const auto committed = m_checkpointStore->commit(consumer, sequence, event.sequence);
        if (!committed.isOk()) return committed;
        sequence = event.sequence;
    }

    // Personality publication is exclusively scheduled by the sleep evolution stage.
    return Result<void, DomainError>::success();
}
