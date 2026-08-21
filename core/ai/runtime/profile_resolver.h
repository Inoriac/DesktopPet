#ifndef DESKTOP_PET_PROFILE_RESOLVER_H
#define DESKTOP_PET_PROFILE_RESOLVER_H

#include <QList>
#include <QString>

#include <optional>

struct PetProfile {
    QString name;
    QString modelPath;
    QString profileId;
};

enum class ProfileResolutionStatus {
    Resolved,
    RegistryUpgradeRequired,
    PetNotFound,
    InvalidProfileId,
    ProfileMismatch
};

struct ProfileResolutionResult {
    ProfileResolutionStatus status = ProfileResolutionStatus::PetNotFound;
    std::optional<PetProfile> profile;
    QString diagnostic;

    bool isResolved() const;
};

class ProfileResolver {
public:
    explicit ProfileResolver(QList<PetProfile> profiles);

    ProfileResolutionResult resolve(
        const QString& petName,
        const std::optional<QString>& requestedProfileId) const;

private:
    QList<PetProfile> m_profiles;
};

#endif // DESKTOP_PET_PROFILE_RESOLVER_H
