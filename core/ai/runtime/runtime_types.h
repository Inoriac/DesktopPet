#ifndef DESKTOP_PET_RUNTIME_TYPES_H
#define DESKTOP_PET_RUNTIME_TYPES_H

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <optional>

#include "profile_data_migrator.h"
#include "profile_resolver.h"

class AIBrain;
class RuntimeUiBridge;

struct AIBrainStorageConfig {
    QString databasePath;
    QString jsonPath;
};

struct RuntimeSnapshot {
    QString sessionId;
    QString profileId;
    int identityBaselineSchemaVersion = 1;
    QString identityBaselineHash;
    std::optional<qint64> personalityVersion;
    std::optional<qint64> relationshipVersion;
    std::optional<QString> selfModelVersion;
    QString configHash;
    QDateTime capturedAt;
};

struct RuntimeStartRequest {
    PetProfile profile;
    ProfileMigrationRequest profileMigration;
    QString configHash;
    int identityBaselineSchemaVersion = 1;
    QString identityBaselineHash;
    AIBrain* aiBrain = nullptr;
    RuntimeUiBridge* uiBridge = nullptr;
};

enum class RuntimeMode {
    Running,
    Degraded
};

struct RuntimeCapabilities {
    bool profileStore = false;
    bool eventLedger = false;
    bool profileGrowth = false;
};

struct RuntimeStartReport {
    RuntimeMode mode = RuntimeMode::Degraded;
    RuntimeCapabilities capabilities;
    ProfileMigrationResult profileMigration;
    QStringList diagnostics;
};

#endif // DESKTOP_PET_RUNTIME_TYPES_H
