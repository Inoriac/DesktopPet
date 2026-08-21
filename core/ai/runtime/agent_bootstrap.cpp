#include "agent_bootstrap.h"

#include <QRegularExpression>

#include "ai/ai_brain.h"
#include "ai/event/event_types.h"
#include "agent_runtime_services.h"
#include "profile_data_migrator.h"

namespace {

bool isSha256(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

} // namespace

Result<RuntimeStartReport, DomainError> AgentBootstrap::start(
    AgentRuntimeServices& services,
    const RuntimeStartRequest& request) {
    if (services.isStarted() || !request.aiBrain || !request.uiBridge
        || !isCanonicalEventUuid(request.profile.profileId)
        || request.profile.name.trimmed().isEmpty()
        || request.profileMigration.profileId != request.profile.profileId
        || request.profileMigration.appDataRoot.trimmed().isEmpty()
        || request.profileMigration.legacyDatabasePath.trimmed().isEmpty()
        || request.profileMigration.legacyJsonPath.trimmed().isEmpty()
        || request.identityBaselineSchemaVersion <= 0
        || !isSha256(request.configHash)
        || !isSha256(request.identityBaselineHash)) {
        return Result<RuntimeStartReport, DomainError>::failure(
            domainError(QStringLiteral("RUNTIME_START_INVALID"),
                        QStringLiteral("agent bootstrap request is invalid")));
    }

    const ProfileMigrationResult migration =
        ProfileDataMigrator().migrateLegacyMemory(request.profileMigration);
    const Result<void, DomainError> storage = request.aiBrain->initializeStorage({
        migration.activeDatabasePath,
        migration.activeJsonPath
    });
    if (!storage.isOk()) {
        return Result<RuntimeStartReport, DomainError>::failure(storage.error());
    }

    Result<RuntimeStartReport, DomainError> started =
        services.startAfterStorageReady(request, migration);
    if (started.isOk()) request.aiBrain->setRuntimeServices(&services);
    return started;
}
