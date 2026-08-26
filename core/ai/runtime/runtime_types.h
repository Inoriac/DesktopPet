#ifndef DESKTOP_PET_RUNTIME_TYPES_H
#define DESKTOP_PET_RUNTIME_TYPES_H

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <optional>

#include "ai_types.h"
#include "ai/identity/identity_baseline.h"
#include "ai/identity/identity_types.h"
#include "profile_data_migrator.h"
#include "profile_resolver.h"

class AIBrain;
class AgentScheduler;
class EmotionStateProvider;
class RuntimeUiBridge;

struct AIBrainStorageConfig {
    QString databasePath;
    QString jsonPath;
};

struct RuntimeSnapshot {
    QString sessionId;
    QString profileId;
    QString subjectId;
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
    IdentityBaseline identityBaseline = IdentityBaseline::defaults();
    PersonalityPolicy personalityPolicy;
    SleepPolicy sleepPolicy;
    QString ownerDiaryBootstrapPath;
    EmotionStateProvider* emotionStateProvider = nullptr;
    AgentScheduler* agentScheduler = nullptr;
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
    bool privateReflection = false;
    bool sleepCycle = false;
    bool ownerDiary = false;
};

struct RuntimeStartReport {
    RuntimeMode mode = RuntimeMode::Degraded;
    RuntimeCapabilities capabilities;
    ProfileMigrationResult profileMigration;
    QStringList diagnostics;
};

#endif // DESKTOP_PET_RUNTIME_TYPES_H
