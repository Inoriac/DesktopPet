#include "identity_types.h"

#include <algorithm>
#include <cmath>
#include <limits>

PersonalityPolicy sanitizePersonalityPolicy(const PersonalityPolicy& policy) {
    const PersonalityPolicy defaults;
    PersonalityPolicy sanitized = policy;
    if (sanitized.minimumIndependentEvidence < 1) {
        sanitized.minimumIndependentEvidence = defaults.minimumIndependentEvidence;
    } else {
        sanitized.minimumIndependentEvidence = std::min(
            sanitized.minimumIndependentEvidence, 64);
    }
    if (sanitized.minimumContextKeys < 1) {
        sanitized.minimumContextKeys = defaults.minimumContextKeys;
    } else {
        sanitized.minimumContextKeys = std::min(sanitized.minimumContextKeys, 16);
    }
    if (sanitized.minimumWindowDays < 1) {
        sanitized.minimumWindowDays = defaults.minimumWindowDays;
    } else {
        sanitized.minimumWindowDays = std::min(sanitized.minimumWindowDays, 365);
    }
    if (!std::isfinite(sanitized.maxDeltaPerWindow)
        || sanitized.maxDeltaPerWindow <= 0.0) {
        sanitized.maxDeltaPerWindow = defaults.maxDeltaPerWindow;
    } else {
        sanitized.maxDeltaPerWindow = std::clamp(
            sanitized.maxDeltaPerWindow, 0.001, 0.25);
    }
    if (sanitized.maxPromptSlotChars <= 0) {
        sanitized.maxPromptSlotChars = defaults.maxPromptSlotChars;
    } else {
        sanitized.maxPromptSlotChars = std::clamp(
            sanitized.maxPromptSlotChars, 64, 4096);
    }
    return sanitized;
}

QString traitEvidenceStatusToString(TraitEvidenceStatus status) {
    switch (status) {
    case TraitEvidenceStatus::Pending:
        return QStringLiteral("Pending");
    case TraitEvidenceStatus::Consumed:
        return QStringLiteral("Consumed");
    case TraitEvidenceStatus::Rejected:
        return QStringLiteral("Rejected");
    }
    return QStringLiteral("Rejected");
}

std::optional<TraitEvidenceStatus> traitEvidenceStatusFromString(
    const QString& value) {
    if (value == QLatin1String("Pending")) return TraitEvidenceStatus::Pending;
    if (value == QLatin1String("Consumed")) return TraitEvidenceStatus::Consumed;
    if (value == QLatin1String("Rejected")) return TraitEvidenceStatus::Rejected;
    return std::nullopt;
}

QString evidenceSourceTypeToString(EvidenceSourceType type) {
    switch (type) {
    case EvidenceSourceType::CommittedEvent:
        return QStringLiteral("committed_event");
    case EvidenceSourceType::DiaryIndex:
        return QStringLiteral("diary_index");
    case EvidenceSourceType::PersonalityVersion:
        return QStringLiteral("personality_version");
    case EvidenceSourceType::SelfModel:
        return QStringLiteral("self_model");
    }
    return QStringLiteral("self_model");
}

std::optional<EvidenceSourceType> evidenceSourceTypeFromString(
    const QString& value) {
    if (value == QLatin1String("committed_event")) {
        return EvidenceSourceType::CommittedEvent;
    }
    if (value == QLatin1String("diary_index")) {
        return EvidenceSourceType::DiaryIndex;
    }
    if (value == QLatin1String("personality_version")) {
        return EvidenceSourceType::PersonalityVersion;
    }
    if (value == QLatin1String("self_model")) {
        return EvidenceSourceType::SelfModel;
    }
    return std::nullopt;
}

QJsonObject numericMapToJson(const QMap<QString, double>& values) {
    QJsonObject object;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (std::isfinite(it.value())) {
            object.insert(it.key(), it.value());
        }
    }
    return object;
}

QMap<QString, double> numericMapFromJson(const QJsonObject& object) {
    QMap<QString, double> values;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const double value = it.value().toDouble(
            std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(value)) {
            values.insert(it.key(), value);
        }
    }
    return values;
}

QJsonObject relationshipSnapshotStateToJson(
    const RelationshipSnapshot& snapshot) {
    QJsonArray sources;
    for (const QString& source : snapshot.sourceEvidenceIds) {
        sources.append(source);
    }
    return {
        {QStringLiteral("tendencies"), numericMapToJson(snapshot.tendencies)},
        {QStringLiteral("sourceEvidenceIds"), sources}
    };
}

RelationshipSnapshot relationshipSnapshotStateFromJson(
    const QJsonObject& object) {
    RelationshipSnapshot snapshot;
    snapshot.tendencies = numericMapFromJson(
        object.value(QStringLiteral("tendencies")).toObject());
    const QJsonArray sources = object.value(
        QStringLiteral("sourceEvidenceIds")).toArray();
    for (const QJsonValue& source : sources) {
        const QString id = source.toString().trimmed();
        if (!id.isEmpty() && !snapshot.sourceEvidenceIds.contains(id)) {
            snapshot.sourceEvidenceIds.append(id);
        }
    }
    return snapshot;
}

QJsonArray evidenceSetToJson(const EvidenceSet& evidence) {
    QJsonArray array;
    for (const SelfModelEvidence& item : evidence.items) {
        array.append(QJsonObject{
            {QStringLiteral("referenceId"), item.referenceId},
            {QStringLiteral("sourceType"), evidenceSourceTypeToString(item.sourceType)},
            {QStringLiteral("committed"), item.committed}
        });
    }
    return array;
}

EvidenceSet evidenceSetFromJson(const QJsonArray& array) {
    EvidenceSet evidence;
    for (const QJsonValue& value : array) {
        if (!value.isObject()) continue;
        const QJsonObject object = value.toObject();
        const QString referenceId = object.value(
            QStringLiteral("referenceId")).toString().trimmed();
        const std::optional<EvidenceSourceType> sourceType =
            evidenceSourceTypeFromString(object.value(
                QStringLiteral("sourceType")).toString());
        if (referenceId.isEmpty() || !sourceType.has_value()) continue;
        evidence.items.append({
            referenceId,
            *sourceType,
            object.value(QStringLiteral("committed")).toBool(false)
        });
    }
    return evidence;
}
