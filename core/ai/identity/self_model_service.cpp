#include "self_model_service.h"

#include <QSet>
#include <QUuid>

#include <utility>

#include "sqlite_identity_repository.h"

SelfModelService::SelfModelService(
    QString profileId, SqliteIdentityRepository* repository)
    : m_profileId(std::move(profileId))
    , m_repository(repository) {}

Result<SelfModelSnapshot, DomainError> SelfModelService::evolve(
    const SelfModelProposal& proposal, const EvidenceSet& evidence) {
    const QString narrative = proposal.narrative.simplified().left(2048);
    if (!m_repository || m_profileId.isEmpty() || narrative.isEmpty()) {
        return Result<SelfModelSnapshot, DomainError>::failure(
            domainError(QStringLiteral("IDENTITY_EVIDENCE_INVALID"),
                        QStringLiteral("self model proposal is invalid")));
    }

    EvidenceSet accepted;
    QSet<QString> references;
    for (const SelfModelEvidence& item : evidence.items) {
        const QString referenceId = item.referenceId.trimmed().left(256);
        if (!item.committed || referenceId.isEmpty()
            || item.sourceType == EvidenceSourceType::SelfModel
            || references.contains(referenceId)) {
            continue;
        }
        references.insert(referenceId);
        accepted.items.append({referenceId, item.sourceType, true});
    }
    if (accepted.items.isEmpty()) {
        return Result<SelfModelSnapshot, DomainError>::failure(
            domainError(QStringLiteral("IDENTITY_EVIDENCE_INVALID"),
                        QStringLiteral("self model requires committed independent evidence")));
    }

    auto currentResult = m_repository->currentSelfModel(m_profileId);
    if (!currentResult.isOk()) {
        return Result<SelfModelSnapshot, DomainError>::failure(currentResult.error());
    }
    const std::optional<SelfModelSnapshot>& current = currentResult.value();
    if (proposal.parentVersionId.has_value()
        && (!current.has_value()
            || current->versionId != proposal.parentVersionId->trimmed())) {
        return Result<SelfModelSnapshot, DomainError>::failure(
            domainError(QStringLiteral("STATE_VERSION_CONFLICT"),
                        QStringLiteral("self model parent changed")));
    }

    SelfModelSnapshot next;
    next.versionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    next.profileId = m_profileId;
    if (current.has_value()) next.parentVersionId = current->versionId;
    next.narrative = narrative;
    next.evidence = std::move(accepted);
    next.effectiveAt = proposal.proposedAt.isValid()
        ? proposal.proposedAt.toUTC() : QDateTime::currentDateTimeUtc();
    next.createdAt = QDateTime::currentDateTimeUtc();

    auto appended = m_repository->appendSelfModelState(next);
    if (!appended.isOk()) {
        return Result<SelfModelSnapshot, DomainError>::failure(appended.error());
    }
    return Result<SelfModelSnapshot, DomainError>::success(std::move(next));
}
