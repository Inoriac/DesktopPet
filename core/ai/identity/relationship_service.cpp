#include "relationship_service.h"

#include <QUuid>

#include <algorithm>
#include <cmath>
#include <utility>

#include "sqlite_identity_repository.h"

RelationshipService::RelationshipService(
    QString profileId, SqliteIdentityRepository* repository)
    : m_profileId(std::move(profileId))
    , m_repository(repository) {}

Result<RelationshipSnapshot, DomainError> RelationshipService::applyEvidence(
    const QString& subjectId, const RelationshipEvidence& evidence) {
    const QString subject = subjectId.trimmed().left(128);
    const QString tendency = evidence.tendency.trimmed().toLower();
    const QString source = evidence.sourceEventId.trimmed().left(256);
    if (!m_repository || m_profileId.isEmpty() || subject.isEmpty()
        || tendency.isEmpty() || tendency.size() > 64 || source.isEmpty()
        || !std::isfinite(evidence.direction)
        || !std::isfinite(evidence.weight)
        || !std::isfinite(evidence.confidence)) {
        return Result<RelationshipSnapshot, DomainError>::failure(
            domainError(QStringLiteral("IDENTITY_EVIDENCE_INVALID"),
                        QStringLiteral("relationship evidence is invalid")));
    }

    auto currentResult = m_repository->currentRelationship(m_profileId, subject);
    if (!currentResult.isOk()) {
        return Result<RelationshipSnapshot, DomainError>::failure(currentResult.error());
    }
    RelationshipSnapshot next;
    if (currentResult.value().has_value()) next = *currentResult.value();
    if (next.sourceEvidenceIds.contains(source)) {
        return Result<RelationshipSnapshot, DomainError>::success(std::move(next));
    }
    next.stateId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    next.profileId = m_profileId;
    next.subjectId = subject;
    next.version += 1;
    const double delta = std::clamp(evidence.direction, -1.0, 1.0)
        * std::clamp(evidence.weight, 0.0, 1.0)
        * std::clamp(evidence.confidence, 0.0, 1.0)
        * 0.10;
    next.tendencies.insert(
        tendency,
        std::clamp(next.tendencies.value(tendency) + delta, -0.5, 0.5));
    next.sourceEvidenceIds.append(source);
    if (next.sourceEvidenceIds.size() > 64) next.sourceEvidenceIds.removeFirst();
    next.effectiveAt = evidence.occurredAt.isValid()
        ? evidence.occurredAt.toUTC() : QDateTime::currentDateTimeUtc();
    next.createdAt = QDateTime::currentDateTimeUtc();

    auto appended = m_repository->appendRelationshipState(next);
    if (!appended.isOk()) {
        return Result<RelationshipSnapshot, DomainError>::failure(appended.error());
    }
    return Result<RelationshipSnapshot, DomainError>::success(std::move(next));
}
