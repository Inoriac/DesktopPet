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
        || sourceEventId.isEmpty() || contextKey.isEmpty()
        || !std::isfinite(evidence.direction)
        || !std::isfinite(evidence.weight)
        || !std::isfinite(evidence.confidence)) {
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
    stored.status = (stored.direction == 0.0
                     || stored.weight == 0.0
                     || stored.confidence == 0.0)
        ? TraitEvidenceStatus::Rejected
        : TraitEvidenceStatus::Pending;
    return m_repository->insertEvidence(stored);
}

Result<PersonalitySnapshot, DomainError> PersonalityService::consolidate(
    const PersonalityConsolidationRequest& request) {
    if (!m_repository || !m_unitOfWorkFactory || !m_eventOutbox
        || request.profileId != m_profileId
        || !request.windowStart.isValid() || !request.windowEnd.isValid()
        || request.windowStart > request.windowEnd
        || request.windowStart.daysTo(request.windowEnd) < m_policy.minimumWindowDays) {
        return Result<PersonalitySnapshot, DomainError>::failure(
            invalidIdentityEvidence(QStringLiteral("personality consolidation request is invalid")));
    }

    auto pendingResult = m_repository->pendingEvidence(
        m_profileId, request.windowStart.toUTC(), request.windowEnd.toUTC());
    if (!pendingResult.isOk()) {
        return Result<PersonalitySnapshot, DomainError>::failure(pendingResult.error());
    }

    QMap<QString, QList<TraitEvidence>> byTrait;
    for (const TraitEvidence& evidence : pendingResult.value()) {
        byTrait[evidence.trait].append(evidence);
    }

    int expectedBaseVersion = request.expectedVersion;
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto currentResult = m_repository->currentPersonality(m_profileId);
        if (!currentResult.isOk()) {
            return Result<PersonalitySnapshot, DomainError>::failure(currentResult.error());
        }
        const PersonalitySnapshot current = currentResult.value().has_value()
            ? *currentResult.value() : baselineSnapshot();
        if (current.version != expectedBaseVersion) {
            if (attempt == 0) {
                expectedBaseVersion = static_cast<int>(current.version);
                continue;
            }
            return Result<PersonalitySnapshot, DomainError>::failure(
                domainError(QStringLiteral("STATE_VERSION_CONFLICT"),
                            QStringLiteral("personality version changed twice")));
        }

        PersonalitySnapshot next = current;
        next.stateId = newId();
        next.version = current.version + 1;
        next.effectiveAt = request.windowEnd.toUTC();
        next.createdAt = QDateTime::currentDateTimeUtc();
        QStringList consumedEvidenceIds;

        for (auto traitIt = byTrait.cbegin(); traitIt != byTrait.cend(); ++traitIt) {
            QSet<QString> sourceIds;
            QSet<QString> contextKeys;
            double signedWeight = 0.0;
            double absoluteWeight = 0.0;
            QStringList candidateIds;
            for (const TraitEvidence& evidence : traitIt.value()) {
                if (sourceIds.contains(evidence.sourceEventId)) continue;
                sourceIds.insert(evidence.sourceEventId);
                contextKeys.insert(evidence.contextKey);
                const double effectiveWeight = evidence.weight * evidence.confidence;
                signedWeight += evidence.direction * effectiveWeight;
                absoluteWeight += std::abs(effectiveWeight);
                candidateIds.append(evidence.evidenceId);
            }
            if (sourceIds.size() < m_policy.minimumIndependentEvidence
                || contextKeys.size() < m_policy.minimumContextKeys
                || absoluteWeight <= 0.0) {
                continue;
            }
            const double delta = std::clamp(
                signedWeight / absoluteWeight * m_policy.maxDeltaPerWindow,
                -m_policy.maxDeltaPerWindow,
                m_policy.maxDeltaPerWindow);
            if (std::abs(delta) < 1e-9) continue;
            next.tendencies.insert(
                traitIt.key(),
                std::clamp(current.tendencies.value(traitIt.key()) + delta,
                           -0.5, 0.5));
            consumedEvidenceIds.append(candidateIds);
            for (const QString& sourceId : sourceIds) {
                if (!next.sourceEvidenceIds.contains(sourceId)) {
                    next.sourceEvidenceIds.append(sourceId);
                }
            }
        }

        if (consumedEvidenceIds.isEmpty()) {
            return Result<PersonalitySnapshot, DomainError>::success(current);
        }

        auto unitOfWorkResult = m_unitOfWorkFactory->begin();
        if (!unitOfWorkResult.isOk()) {
            return Result<PersonalitySnapshot, DomainError>::failure(
                unitOfWorkResult.error());
        }
        std::unique_ptr<RuntimeUnitOfWork> unitOfWork = unitOfWorkResult.takeValue();
        auto appended = m_repository->appendPersonalityState(
            *unitOfWork, next, consumedEvidenceIds);
        if (!appended.isOk()) {
            unitOfWork->rollback();
            if (appended.error().code == QLatin1String("STATE_VERSION_CONFLICT")
                && attempt == 0) {
                auto latest = m_repository->currentPersonality(m_profileId);
                if (!latest.isOk()) {
                    return Result<PersonalitySnapshot, DomainError>::failure(latest.error());
                }
                expectedBaseVersion = latest.value().has_value()
                    ? static_cast<int>(latest.value()->version) : 0;
                continue;
            }
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
        auto enqueued = m_eventOutbox->enqueue(*unitOfWork, changed);
        if (!enqueued.isOk()) {
            unitOfWork->rollback();
            return Result<PersonalitySnapshot, DomainError>::failure(enqueued.error());
        }
        auto committed = unitOfWork->commit();
        if (!committed.isOk()) {
            return Result<PersonalitySnapshot, DomainError>::failure(committed.error());
        }
        return Result<PersonalitySnapshot, DomainError>::success(std::move(next));
    }

    return Result<PersonalitySnapshot, DomainError>::failure(
        domainError(QStringLiteral("STATE_VERSION_CONFLICT"),
                    QStringLiteral("personality version changed twice")));
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
    restored.version = currentVersion + 1;
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
