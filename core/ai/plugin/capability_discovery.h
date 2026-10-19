#pragma once
#include "capability_types.h"

// Pure discovery over the host's already-authorized snapshot.
Result<QJsonObject, DomainError> queryCapabilityEntries(
    const QJsonArray& entries, const QJsonObject& query, const QString& snapshotKey);
QJsonArray selectCapabilityCandidates(const QJsonArray& entries,
    const QString& query, quint64 browseOffset, int limit = 16);
