#include "personality_service.h"

#include <QMap>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <utility>

#include "ai/event/event_outbox.h"
#include "ai/event/runtime_unit_of_work.h"
#include "sqlite_identity_repository.h"

namespace {

DomainError invalidIdentityEvidence(const QString& message) {
    return domainError(QStringLiteral("IDENTITY_EVIDENCE_INVALID"), message);
}

bool isTraitNameValid(const QString& name) {
    if (name.isEmpty() || name.size() > 64) return false;
    for (const QChar character : name) {
        if (!character.isLetterOrNumber()
            && character != QLatin1Char('_')
            && character != QLatin1Char('-')) {
            return false;
        }
    }
    return true;
}

QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

} // namespace

PersonalityService::PersonalityService(
    QString profileId,
    IdentityBaseline baseline,
    PersonalityPolicy policy,
    SqliteIdentityRepository* repository,
    RuntimeUnitOfWorkFactory* unitOfWorkFactory,
    EventOutbox* eventOutbox)
    : m_profileId(std::move(profileId))
    , m_baseline(std::move(baseline))
    , m_policy(sanitizePersonalityPolicy(policy))
    , m_repository(repository)
    , m_unitOfWorkFactory(unitOfWorkFactory)
    , m_eventOutbox(eventOutbox) {}

PersonalitySnapshot PersonalityService::baselineSnapshot() const {
    PersonalitySnapshot snapshot;
    snapshot.profileId = m_profileId;
    snapshot.baseline = m_baseline;
    return snapshot;
}

Result<void, DomainError> PersonalityService::recordEvidence(
    const TraitEvidenceDraft& evidence) {
    const QString trait = evidence.trait.trimmed().toLower();
    const QString sourceEventId = evidence.sourceEventId.trimmed();
    const QString contextKey = evidence.contextKey.trimmed().left(128);
    if (!m_repository || m_profileId.isEmpty() || !isTraitNameValid(trait)
        || traitConsolidationLimit(trait) <= 0.0
        || sourceEventId.isEmpty() || contextKey.isEmpty()
        || !std::isfinite(evidence.direction)
        || !std::isfinite(evidence.weight)
        || !std::isfinite(evidence.confidence)
        || evidence.sourceSequence < 0
        || (evidence.sourceCategory != QLatin1String("interaction")
            && evidence.originalSourceId.trimmed().isEmpty())) {
        return Result<void, DomainError>::failure(
            invalidIdentityEvidence(QStringLiteral("trait evidence is invalid")));
    }

    TraitEvidence stored;
    stored.evidenceId = newId();
    stored.profileId = m_profileId;
    stored.trait = trait;
    stored.direction = std::clamp(evidence.direction, -1.0, 1.0);
    stored.weight = std::clamp(evidence.weight, 0.0, 1.0);
    stored.confidence = std::clamp(evidence.confidence, 0.0, 1.0);
    stored.contextKey = contextKey;
    stored.sourceEventId = sourceEventId.left(256);
    stored.createdAt = evidence.createdAt.isValid()
        ? evidence.createdAt.toUTC() : QDateTime::currentDateTimeUtc();
    stored.originalSourceId = evidence.originalSourceId.trimmed().isEmpty()
        ? stored.sourceEventId : evidence.originalSourceId.trimmed().left(256);
    stored.originalOccurredAt = evidence.originalOccurredAt.isValid()
        ? evidence.originalOccurredAt.toUTC() : stored.createdAt;
    stored.sourceCategory = evidence.sourceCategory.trimmed().left(64);
    stored.category = evidence.category;
    stored.sourceSequence = evidence.sourceSequence;
    stored.status = (stored.direction == 0.0
                     || stored.weight == 0.0
                     || stored.confidence == 0.0)
        ? TraitEvidenceStatus::Rejected
        : TraitEvidenceStatus::Pending;
    return m_repository->insertEvidence(stored);
}

Result<PersonalityConsolidationPreview, DomainError> PersonalityService::preview(
    const PersonalityConsolidationRequest& request) const {
    if (!m_repository || !m_unitOfWorkFactory || !m_eventOutbox
        || request.profileId != m_profileId || request.expectedVersion < 0
        || request.evidenceCutoffSequence < 0
        || !request.windowStart.isValid() || !request.windowEnd.isValid()
        || request.windowStart > request.windowEnd
        || request.windowStart.secsTo(request.windowEnd) < qint64(m_policy.minimumWindowDays) * 86400) {
        return Result<PersonalityConsolidationPreview, DomainError>::failure(
            invalidIdentityEvidence(QStringLiteral("personality consolidation request is invalid")));
    }
    auto currentResult = m_repository->currentPersonality(m_profileId);
    if (!currentResult.isOk()) {
        return Result<PersonalityConsolidationPreview, DomainError>::failure(currentResult.error());
    }
    PersonalityConsolidationPreview result;
    result.request = request;
    result.current = currentResult.value().value_or(baselineSnapshot());
    result.candidate = result.current;
    if (result.current.version != request.expectedVersion) {
        return Result<PersonalityConsolidationPreview, DomainError>::failure(
            domainError(QStringLiteral("STATE_VERSION_CONFLICT"),
                        QStringLiteral("personality changed; reread materials and re-evaluate the proposal")));
    }
    if (result.current.effectiveAt.isValid()
        && result.current.effectiveAt.secsTo(request.windowEnd) < qint64(m_policy.minimumWindowDays) * 86400) {
        return Result<PersonalityConsolidationPreview, DomainError>::success(std::move(result));
    }
    auto pending = m_repository->pendingEvidence(
        m_profileId, request.windowStart.toUTC(), request.windowEnd.toUTC());
    if (!pending.isOk()) {
        return Result<PersonalityConsolidationPreview, DomainError>::failure(pending.error());
    }
    QMap<QString, QList<TraitEvidence>> byTrait;
    result.candidate.sourceEvidenceRevisions.clear();
    for (const auto& evidence : pending.value()) {
        if (request.evidenceCutoffSequence > 0
            && evidence.sourceSequence > request.evidenceCutoffSequence) continue;
        byTrait[evidence.trait].append(evidence);
    }
    for (auto it = byTrait.cbegin(); it != byTrait.cend(); ++it) {
        const double limit = std::min(traitConsolidationLimit(it.key()), m_policy.maxDeltaPerWindow);
        if (limit <= 0.0) continue;
        QSet<QString> originals;
        QSet<QString> contexts;
        QDateTime first;
        QDateTime last;
        double signedWeight = 0.0;
        double totalWeight = 0.0;
        QStringList consumed;
        QMap<QString, qint64> revisions;
        for (const auto& evidence : it.value()) {
            if (originals.contains(evidence.originalSourceId)) continue;
            originals.insert(evidence.originalSourceId);
            contexts.insert(evidence.contextKey);
            if (!first.isValid() || evidence.originalOccurredAt < first) first = evidence.originalOccurredAt;
            if (!last.isValid() || evidence.originalOccurredAt > last) last = evidence.originalOccurredAt;
            const double weight = evidence.weight * evidence.confidence;
            signedWeight += evidence.direction * weight;
            totalWeight += weight;
            consumed.append(evidence.evidenceId);
            revisions.insert(evidence.evidenceId, evidence.revision);
        }
        if (originals.size() < m_policy.minimumIndependentEvidence
            || contexts.size() < m_policy.minimumContextKeys || totalWeight <= 0.0
            || !first.isValid() || !last.isValid()
            || first.secsTo(last) < qint64(m_policy.minimumWindowDays) * 86400) continue;
        const double previous = result.current.tendencies.value(it.key());
        const double next = std::clamp(previous + std::clamp(signedWeight / totalWeight, -1.0, 1.0) * limit,
                                       -0.30, 0.30);
        if (std::abs(next - previous) < 1e-9) continue;
        result.candidate.tendencies.insert(it.key(), next);
        result.consumedEvidenceIds.append(consumed);
        result.candidate.sourceEvidenceRevisions.insert(revisions);
    }
    if (result.consumedEvidenceIds.isEmpty()) {
        return Result<PersonalityConsolidationPreview, DomainError>::success(std::move(result));
    }
    result.hasChanges = true;
    result.candidate.stateId = newId();
    result.candidate.parentVersion = result.current.version;
    result.candidate.version = result.current.version + 1;
    result.candidate.effectiveAt = request.windowEnd.toUTC();
    result.candidate.createdAt = QDateTime::currentDateTimeUtc();
    result.candidate.evidenceCutoffSequence = request.evidenceCutoffSequence;
    result.candidate.sourceEvidenceIds = result.consumedEvidenceIds;
    result.candidate.behaviorStyleSummary.clear();
    result.candidate.selfNarrativeSummary.clear();
    return Result<PersonalityConsolidationPreview, DomainError>::success(std::move(result));
}

QString PersonalityService::deterministicBehaviorSummary(const PersonalitySnapshot& snapshot) {
    const auto traits = snapshot.effectiveTraits();
    QStringList descriptions;
    const QMap<QString, QPair<QString, QString>> labels{
        {QStringLiteral("initiative"), {QStringLiteral("适时提出建议"), QStringLiteral("留出安静空间")}},
        {QStringLiteral("sociability"), {QStringLiteral("乐于交流"), QStringLiteral("表达简洁温和")}},
        {QStringLiteral("openness"), {QStringLiteral("愿意探索陌生问题"), QStringLiteral("优先熟悉的思路")}},
        {QStringLiteral("persistence"), {QStringLiteral("遇到困难愿意继续尝试"), QStringLiteral("及时调整方向")}},
        {QStringLiteral("caution"), {QStringLiteral("重视证据并说明不确定性"), QStringLiteral("愿意尝试并核验结果")}},
        {QStringLiteral("autonomy"), {QStringLiteral("坦诚协商并表达边界"), QStringLiteral("温和说明需要与限制")}},
        {QStringLiteral("helpfulness"), {QStringLiteral("主动理解并帮助用户"), QStringLiteral("尊重用户自行处理的空间")}}
    };
    for (auto it = traits.cbegin(); it != traits.cend(); ++it) {
        const auto label = labels.value(it.key());
        descriptions.append(it.value() >= 0.5 ? label.first : label.second);
    }
    return descriptions.join(QStringLiteral("；")) + QStringLiteral("。拒绝时说明真实理由，自我描述以已确认经历为准。");
}

QString PersonalityService::deterministicNarrativeSummary(const PersonalitySnapshot& snapshot) {
    return QStringLiteral("我根据跨场景的长期经历逐渐调整相处方式；感受和推测仍是主观理解。当前我会%1")
        .arg(deterministicBehaviorSummary(snapshot));
}

Result<PersonalitySnapshot, DomainError> PersonalityService::commit(
    const PersonalityConsolidationPreview& evaluated,
    const QString& behaviorStyleSummary,
    const QString& selfNarrativeSummary) {
    // Revalidate the candidate after the model calls, before acquiring any transaction.
    const auto fresh = preview(evaluated.request);
    if (!fresh.isOk()) return Result<PersonalitySnapshot, DomainError>::failure(fresh.error());
    if (fresh.value().hasChanges != evaluated.hasChanges
        || fresh.value().candidate.tendencies != evaluated.candidate.tendencies
        || fresh.value().candidate.sourceEvidenceRevisions != evaluated.candidate.sourceEvidenceRevisions
        || fresh.value().consumedEvidenceIds != evaluated.consumedEvidenceIds) {
        return Result<PersonalitySnapshot, DomainError>::failure(
            domainError(QStringLiteral("STATE_VERSION_CONFLICT"),
                        QStringLiteral("consolidation materials changed; regenerate the proposal and summaries")));
    }
    if (!fresh.value().hasChanges) {
        return Result<PersonalitySnapshot, DomainError>::success(fresh.value().current);
    }
    PersonalitySnapshot next = fresh.value().candidate;
    const QString style = behaviorStyleSummary.simplified();
    const QString narrative = selfNarrativeSummary.simplified();
    next.behaviorStyleSummary = !style.isEmpty() && style.size() <= 2048
        ? style : deterministicBehaviorSummary(next);
    next.selfNarrativeSummary = !narrative.isEmpty() && narrative.size() <= 2048
        ? narrative : deterministicNarrativeSummary(next);
    auto unitOfWorkResult = m_unitOfWorkFactory->begin();
    if (!unitOfWorkResult.isOk()) {
        return Result<PersonalitySnapshot, DomainError>::failure(unitOfWorkResult.error());
    }
    auto unitOfWork = unitOfWorkResult.takeValue();
    const auto appended = m_repository->appendPersonalityState(
        *unitOfWork, next, fresh.value().consumedEvidenceIds);
    if (!appended.isOk()) {
        unitOfWork->rollback();
        return Result<PersonalitySnapshot, DomainError>::failure(appended.error());
    }
    EventDraft changed;
    changed.profileId = m_profileId;
    changed.type = QStringLiteral("PersonalityChanged");
    changed.source = QStringLiteral("PersonalityService");
    changed.payload = {
        {QStringLiteral("stateId"), next.stateId},
        {QStringLiteral("version"), static_cast<double>(next.version)},
        {QStringLiteral("operation"), QStringLiteral("consolidate")},
        {QStringLiteral("reason"), QStringLiteral("independent evidence window")}
    };
    const auto enqueued = m_eventOutbox->enqueue(*unitOfWork, changed);
    if (!enqueued.isOk()) {
        unitOfWork->rollback();
        return Result<PersonalitySnapshot, DomainError>::failure(enqueued.error());
    }
    const auto committed = unitOfWork->commit();
    if (!committed.isOk()) return Result<PersonalitySnapshot, DomainError>::failure(committed.error());
    return Result<PersonalitySnapshot, DomainError>::success(std::move(next));
}

Result<PersonalitySnapshot, DomainError> PersonalityService::consolidate(
    const PersonalityConsolidationRequest& request) {
    const auto evaluated = preview(request);
    if (!evaluated.isOk()) return Result<PersonalitySnapshot, DomainError>::failure(evaluated.error());
    return commit(evaluated.value(), {}, {});
}

Result<PersonalitySnapshot, DomainError> PersonalityService::rollback(
    const QString& targetStateId,
    int expectedVersion,
    const QString& reason) {
    if (!m_repository || !m_unitOfWorkFactory || !m_eventOutbox
        || targetStateId.trimmed().isEmpty() || reason.trimmed().isEmpty()) {
        return Result<PersonalitySnapshot, DomainError>::failure(
            invalidIdentityEvidence(QStringLiteral("personality rollback request is invalid")));
    }
    auto targetResult = m_repository->personalityByStateId(
        m_profileId, targetStateId.trimmed());
    if (!targetResult.isOk()) {
        return Result<PersonalitySnapshot, DomainError>::failure(targetResult.error());
    }
    if (!targetResult.value().has_value()) {
        return Result<PersonalitySnapshot, DomainError>::failure(
            invalidIdentityEvidence(QStringLiteral("rollback target does not exist")));
    }
    auto currentResult = m_repository->currentPersonality(m_profileId);
    if (!currentResult.isOk()) {
        return Result<PersonalitySnapshot, DomainError>::failure(currentResult.error());
    }
    const qint64 currentVersion = currentResult.value().has_value()
        ? currentResult.value()->version : 0;
    if (currentVersion != expectedVersion) {
        return Result<PersonalitySnapshot, DomainError>::failure(
            domainError(QStringLiteral("STATE_VERSION_CONFLICT"),
                        QStringLiteral("personality rollback version changed")));
    }

    PersonalitySnapshot restored = *targetResult.value();
    restored.stateId = newId();
    restored.parentVersion = currentVersion;
    restored.version = currentVersion + 1;
    if (restored.behaviorStyleSummary.isEmpty()) restored.behaviorStyleSummary = deterministicBehaviorSummary(restored);
    if (restored.selfNarrativeSummary.isEmpty()) restored.selfNarrativeSummary = deterministicNarrativeSummary(restored);
    restored.effectiveAt = QDateTime::currentDateTimeUtc();
    restored.createdAt = restored.effectiveAt;

    auto unitOfWorkResult = m_unitOfWorkFactory->begin();
    if (!unitOfWorkResult.isOk()) {
        return Result<PersonalitySnapshot, DomainError>::failure(unitOfWorkResult.error());
    }
    std::unique_ptr<RuntimeUnitOfWork> unitOfWork = unitOfWorkResult.takeValue();
    auto appended = m_repository->appendPersonalityState(*unitOfWork, restored, {});
    if (!appended.isOk()) {
        unitOfWork->rollback();
        return Result<PersonalitySnapshot, DomainError>::failure(appended.error());
    }
    EventDraft changed;
    changed.profileId = m_profileId;
    changed.type = QStringLiteral("PersonalityChanged");
    changed.source = QStringLiteral("PersonalityService");
    changed.payload = {
        {QStringLiteral("stateId"), restored.stateId},
        {QStringLiteral("version"), static_cast<double>(restored.version)},
        {QStringLiteral("operation"), QStringLiteral("rollback")},
        {QStringLiteral("reason"), reason.simplified().left(256)}
    };
    auto enqueued = m_eventOutbox->enqueue(*unitOfWork, changed);
    if (!enqueued.isOk()) {
        unitOfWork->rollback();
        return Result<PersonalitySnapshot, DomainError>::failure(enqueued.error());
    }
    auto committed = unitOfWork->commit();
    if (!committed.isOk()) {
        return Result<PersonalitySnapshot, DomainError>::failure(committed.error());
    }
    return Result<PersonalitySnapshot, DomainError>::success(std::move(restored));
}
