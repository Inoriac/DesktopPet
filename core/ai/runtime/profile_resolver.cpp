#include "profile_resolver.h"

#include <QSet>
#include <QUuid>

#include <utility>

namespace {

std::optional<QString> canonicalProfileId(const QString& value) {
    const QString candidate = value.trimmed();
    const QUuid uuid = QUuid::fromString(candidate);
    if (uuid.isNull()) return std::nullopt;
    const QString canonical = uuid.toString(QUuid::WithoutBraces).toLower();
    if (candidate != canonical) return std::nullopt;
    return canonical;
}

ProfileResolutionResult failure(ProfileResolutionStatus status, const QString& diagnostic) {
    return {status, std::nullopt, diagnostic};
}

} // namespace

bool ProfileResolutionResult::isResolved() const {
    return status == ProfileResolutionStatus::Resolved && profile.has_value();
}

ProfileResolver::ProfileResolver(QList<PetProfile> profiles)
    : m_profiles(std::move(profiles)) {}

ProfileResolutionResult ProfileResolver::resolve(
    const QString& petName,
    const std::optional<QString>& requestedProfileId) const {
    QSet<QString> names;
    QSet<QString> profileIds;
    for (const PetProfile& profile : m_profiles) {
        if (profile.name.trimmed().isEmpty() || profile.modelPath.trimmed().isEmpty()
            || names.contains(profile.name)) {
            return failure(ProfileResolutionStatus::InvalidProfileId,
                           QStringLiteral("pets.json contains invalid or duplicate profile fields"));
        }
        names.insert(profile.name);
        if (profile.profileId.trimmed().isEmpty()) {
            return failure(ProfileResolutionStatus::RegistryUpgradeRequired,
                           QStringLiteral("pets.json must be upgraded by the launcher"));
        }
        const std::optional<QString> canonical = canonicalProfileId(profile.profileId);
        if (!canonical || profileIds.contains(*canonical)) {
            return failure(ProfileResolutionStatus::InvalidProfileId,
                           QStringLiteral("pets.json contains an invalid or duplicate profile mapping"));
        }
        profileIds.insert(*canonical);
    }

    const QString requestedName = petName.trimmed();
    const PetProfile* selected = nullptr;

    if (requestedName.isEmpty()) {
        if (!m_profiles.isEmpty()) selected = &m_profiles.first();
    } else {
        for (const PetProfile& profile : m_profiles) {
            if (profile.name == requestedName) {
                selected = &profile;
                break;
            }
        }
    }
    if (!selected) {
        return failure(ProfileResolutionStatus::PetNotFound,
                       QStringLiteral("pet profile was not found"));
    }
    const std::optional<QString> registryId = canonicalProfileId(selected->profileId);
    if (!registryId) {
        return failure(ProfileResolutionStatus::InvalidProfileId,
                       QStringLiteral("registry profileId is not a canonical UUID"));
    }
    if (requestedProfileId.has_value()) {
        const std::optional<QString> argumentId = canonicalProfileId(*requestedProfileId);
        if (!argumentId) {
            return failure(ProfileResolutionStatus::InvalidProfileId,
                           QStringLiteral("--profile-id is not a canonical UUID"));
        }
        if (*argumentId != *registryId) {
            return failure(ProfileResolutionStatus::ProfileMismatch,
                           QStringLiteral("--pet and --profile-id refer to different profiles"));
        }
    }

    PetProfile resolved = *selected;
    resolved.profileId = *registryId;
    return {ProfileResolutionStatus::Resolved, resolved, QString()};
}
