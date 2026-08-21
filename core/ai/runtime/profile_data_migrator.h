#ifndef DESKTOP_PET_PROFILE_DATA_MIGRATOR_H
#define DESKTOP_PET_PROFILE_DATA_MIGRATOR_H

#include <QString>
#include <QStringList>

#include <optional>

enum class ProfileMigrationStatus {
    NoLegacyData,
    AlreadyMigrated,
    Migrated,
    Ambiguous,
    Failed
};

struct ProfileMigrationRequest {
    QString profileId;
    QStringList registeredProfileIds;
    std::optional<QString> confirmedLegacyOwnerProfileId;
    QString appDataRoot;
    QString legacyDatabasePath;
    QString legacyJsonPath;
};

struct ProfileMigrationResult {
    ProfileMigrationStatus status = ProfileMigrationStatus::Failed;
    QString activeDatabasePath;
    QString activeJsonPath;
    QString diagnostic;

    bool profileStoreReady() const;
};

class ProfileDataMigrator {
public:
    ProfileMigrationResult migrateLegacyMemory(
        const ProfileMigrationRequest& request) const;
};

#endif // DESKTOP_PET_PROFILE_DATA_MIGRATOR_H
