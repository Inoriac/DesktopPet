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
            std::max(sanitized.minimumIndependentEvidence, 3), 64);
    }
    if (sanitized.minimumContextKeys < 1) {
        sanitized.minimumContextKeys = defaults.minimumContextKeys;
    } else {
        sanitized.minimumContextKeys = std::clamp(sanitized.minimumContextKeys, 2, 16);
    }
    if (sanitized.minimumWindowDays < 1) {
        sanitized.minimumWindowDays = defaults.minimumWindowDays;
    } else {
        sanitized.minimumWindowDays = std::clamp(sanitized.minimumWindowDays, 14, 365);
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

QString traitEvidenceCategoryToString(TraitEvidenceCategory category) {
    switch (category) {
    case TraitEvidenceCategory::Fact: return QStringLiteral("fact");
    case TraitEvidenceCategory::Feeling: return QStringLiteral("feeling");
    case TraitEvidenceCategory::Speculation: return QStringLiteral("speculation");
    }
    return QStringLiteral("speculation");
}

std::optional<TraitEvidenceCategory> traitEvidenceCategoryFromString(const QString& value) {
    if (value == QLatin1String("fact")) return TraitEvidenceCategory::Fact;
    if (value == QLatin1String("feeling")) return TraitEvidenceCategory::Feeling;
    if (value == QLatin1String("speculation")) return TraitEvidenceCategory::Speculation;
    return std::nullopt;
}

double traitConsolidationLimit(const QString& trait) {
    if (trait == QLatin1String("initiative") || trait == QLatin1String("sociability")
        || trait == QLatin1String("persistence")) return 0.04;
    if (trait == QLatin1String("openness")) return 0.05;
    if (trait == QLatin1String("caution") || trait == QLatin1String("autonomy")) return 0.03;
    if (trait == QLatin1String("helpfulness")) return 0.02;
    return 0.0;
}

QMap<QString, double> PersonalitySnapshot::effectiveTraits() const {
    QMap<QString, double> values = IdentityBaseline::defaults().traits;
    for (auto it = values.begin(); it != values.end(); ++it) {
        it.value() = std::clamp(baseline.traits.value(it.key(), it.value())
            + std::clamp(tendencies.value(it.key()), -0.30, 0.30), 0.05, 0.95);
    }
    return values;
}

QMap<QString, double> relationshipDefaults() {
    return {{QStringLiteral("affinity"), 0.50}, {QStringLiteral("trust"), 0.45},
            {QStringLiteral("familiarity"), 0.10}, {QStringLiteral("boundaryRespect"), 0.60}};
}

double RelationshipSnapshot::favorability() const {
    return 0.4 * tendencies.value(QStringLiteral("affinity"), 0.50)
        + 0.3 * tendencies.value(QStringLiteral("trust"), 0.45)
        + 0.2 * tendencies.value(QStringLiteral("familiarity"), 0.10)
        + 0.1 * tendencies.value(QStringLiteral("boundaryRespect"), 0.60);
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
        {QStringLiteral("dailyAbsoluteChanges"), numericMapToJson(snapshot.dailyAbsoluteChanges)},
        {QStringLiteral("sourceEvidenceIds"), sources}
    };
}

RelationshipSnapshot relationshipSnapshotStateFromJson(
    const QJsonObject& object) {
    RelationshipSnapshot snapshot;
    const auto stored = numericMapFromJson(object.value(QStringLiteral("tendencies")).toObject());
    for (auto it = snapshot.tendencies.begin(); it != snapshot.tendencies.end(); ++it) {
        it.value() = std::clamp(stored.value(it.key(), it.value()), 0.0, 1.0);
    }
    snapshot.dailyAbsoluteChanges = numericMapFromJson(
        object.value(QStringLiteral("dailyAbsoluteChanges")).toObject());
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

QString originKindToString(OriginKind kind) {
    switch (kind) {
    case OriginKind::LocalOwner: return QStringLiteral("local_owner");
    case OriginKind::External: return QStringLiteral("external");
    case OriginKind::Internal: return QStringLiteral("internal");
    }
    return QStringLiteral("internal");
}

std::optional<OriginKind> originKindFromString(const QString& value) {
    if (value == QLatin1String("local_owner")) return OriginKind::LocalOwner;
    if (value == QLatin1String("external")) return OriginKind::External;
    if (value == QLatin1String("internal")) return OriginKind::Internal;
    return std::nullopt;
}
