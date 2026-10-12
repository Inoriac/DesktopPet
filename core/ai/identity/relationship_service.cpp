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
    QString tendency = evidence.tendency.trimmed().toLower();
    if (tendency == QLatin1String("boundaryrespect")) tendency = QStringLiteral("boundaryRespect");
    const QString source = evidence.sourceEventId.trimmed().left(256);
    if (!m_repository || m_profileId.isEmpty() || subject.isEmpty()
        || !relationshipDefaults().contains(tendency) || source.isEmpty()
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
    next.profileId = m_profileId;
    next.subjectId = subject;
    const QString sourceKey = source + QLatin1Char(':') + tendency;
    if (next.sourceEvidenceIds.contains(sourceKey)
        || evidence.eventKind == QLatin1String("inactivity")
        || evidence.eventKind == QLatin1String("declined")) {
        return Result<RelationshipSnapshot, DomainError>::success(std::move(next));
    }
    next.stateId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    next.profileId = m_profileId;
    next.subjectId = subject;
    next.version += 1;
    next.effectiveAt = evidence.occurredAt.isValid()
        ? evidence.occurredAt.toUTC() : QDateTime::currentDateTimeUtc();
    const QString dayKey = next.effectiveAt.date().toString(Qt::ISODate) + QLatin1Char(':') + tendency;
    const double remaining = std::max(0.0, 0.05 - next.dailyAbsoluteChanges.value(dayKey));
    const double proposed = std::clamp(evidence.direction, -1.0, 1.0)
        * std::clamp(evidence.weight, 0.0, 1.0)
        * std::clamp(evidence.confidence, 0.0, 1.0)
        * 0.02;
    const double previous = next.tendencies.value(tendency);
    const double changed = std::clamp(previous + std::clamp(proposed, -remaining, remaining), 0.0, 1.0);
    next.tendencies.insert(tendency, changed);
    next.dailyAbsoluteChanges[dayKey] += std::abs(changed - previous);
    next.sourceEvidenceIds.append(sourceKey);
    next.createdAt = QDateTime::currentDateTimeUtc();

    auto appended = m_repository->appendRelationshipState(next);
    if (!appended.isOk()) {
        return Result<RelationshipSnapshot, DomainError>::failure(appended.error());
    }
    return Result<RelationshipSnapshot, DomainError>::success(std::move(next));
}
