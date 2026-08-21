#include "agent_runtime_services.h"

#include <QCryptographicHash>
#include <QDir>

#include "ai/ai_brain.h"
#include "ai/event/event_ledger.h"
#include "ai/event/event_outbox.h"
#include "ai/event/event_schema_registry.h"
#include "ai/event/runtime_unit_of_work.h"
#include "ai/event/sqlite_event_repository.h"
#include "runtime_ui_bridge.h"

AgentRuntimeServices::~AgentRuntimeServices() {
    stop();
}

Result<RuntimeStartReport, DomainError> AgentRuntimeServices::startAfterStorageReady(
    const RuntimeStartRequest& request,
    const ProfileMigrationResult& migration) {
    if (m_started || !request.aiBrain || !request.uiBridge
        || request.profile.profileId != request.profileMigration.profileId) {
        return Result<RuntimeStartReport, DomainError>::failure(
            domainError(QStringLiteral("RUNTIME_START_INVALID"),
                        QStringLiteral("runtime services start request is invalid")));
    }

    m_profileId = request.profile.profileId;
    m_configHash = request.configHash;
    m_identityBaselineSchemaVersion = request.identityBaselineSchemaVersion;
    m_identityBaselineHash = request.identityBaselineHash;
    m_aiBrain = request.aiBrain;
    m_uiBridge = request.uiBridge;
    m_started = true;

    RuntimeStartReport report;
    report.profileMigration = migration;
    report.capabilities.profileStore = migration.profileStoreReady();
    report.capabilities.profileGrowth = migration.profileStoreReady();
    if (!migration.diagnostic.isEmpty()) report.diagnostics.append(migration.diagnostic);

    const QString runtimeDatabasePath = QDir(request.profileMigration.appDataRoot)
        .filePath(QStringLiteral("profiles/%1/agent_runtime.sqlite").arg(m_profileId));
    m_eventSchemas = std::make_unique<EventSchemaRegistry>();
    const Result<void, DomainError> schemas = registerBuiltInEventSchemas(*m_eventSchemas);
    if (!schemas.isOk()) {
        report.diagnostics.append(schemas.error().message);
        m_eventSchemas.reset();
        report.mode = RuntimeMode::Degraded;
        return Result<RuntimeStartReport, DomainError>::success(std::move(report));
    }
    m_eventSchemas->freeze();

    m_eventRepository = std::make_unique<SqliteEventRepository>();
    const Result<void, DomainError> opened = m_eventRepository->open(runtimeDatabasePath);
    if (!opened.isOk()) {
        report.diagnostics.append(opened.error().message);
        m_eventRepository.reset();
        m_eventSchemas.reset();
        report.mode = RuntimeMode::Degraded;
        return Result<RuntimeStartReport, DomainError>::success(std::move(report));
    }

    m_eventLedger = std::make_unique<SqliteEventLedger>(
        m_eventRepository.get(), m_eventSchemas.get(), m_profileId);
    m_checkpointStore = std::make_unique<SqliteEventConsumerCheckpointStore>(
        m_eventRepository.get());
    m_unitOfWorkFactory = std::make_unique<SqliteRuntimeUnitOfWorkFactory>(
        runtimeDatabasePath);
    m_eventOutbox = std::make_unique<SqliteEventOutbox>(
        runtimeDatabasePath, m_eventSchemas.get(), m_profileId);
    report.capabilities.eventLedger = true;
    report.mode = report.capabilities.profileGrowth
        ? RuntimeMode::Running
        : RuntimeMode::Degraded;

    if (!report.capabilities.profileGrowth) {
        EventDraft degraded;
        degraded.profileId = m_profileId;
        degraded.type = QStringLiteral("RuntimeDegraded");
        degraded.source = QStringLiteral("AgentBootstrap");
        degraded.payload = {
            {QStringLiteral("capability"), QStringLiteral("profileGrowth")},
            {QStringLiteral("reasonCode"),
             migration.status == ProfileMigrationStatus::Ambiguous
                 ? QStringLiteral("PROFILE_MIGRATION_AMBIGUOUS")
                 : QStringLiteral("PROFILE_MIGRATION_FAILED")}
        };
        if (!migration.diagnostic.isEmpty()) {
            degraded.payload.insert(
                QStringLiteral("diagnosticHash"),
                QString::fromLatin1(QCryptographicHash::hash(
                    migration.diagnostic.toUtf8(), QCryptographicHash::Sha256).toHex()));
        }
        m_eventLedger->append(degraded);
    }
    return Result<RuntimeStartReport, DomainError>::success(std::move(report));
}

RuntimeSnapshot AgentRuntimeServices::captureSnapshot(const QString& sessionId) const {
    if (!m_started || sessionId.trimmed().isEmpty()) return {};
    RuntimeSnapshot snapshot;
    snapshot.sessionId = sessionId;
    snapshot.profileId = m_profileId;
    snapshot.identityBaselineSchemaVersion = m_identityBaselineSchemaVersion;
    snapshot.identityBaselineHash = m_identityBaselineHash;
    snapshot.configHash = m_configHash;
    snapshot.capturedAt = QDateTime::currentDateTimeUtc();
    return snapshot;
}

Result<EventReadAuthorization, DomainError> AgentRuntimeServices::authorizationFor(
    const QString& consumerId) const {
    if (!m_started) {
        return Result<EventReadAuthorization, DomainError>::failure(
            domainError(QStringLiteral("EVT_READ_FORBIDDEN"),
                        QStringLiteral("runtime services are stopped")));
    }
    if (consumerId == QLatin1String("identity")) {
        return Result<EventReadAuthorization, DomainError>::success(
            EventReadAuthorization(consumerId, m_profileId, true, {}));
    }
    if (consumerId == QLatin1String("reflection")) {
        return Result<EventReadAuthorization, DomainError>::success(
            EventReadAuthorization(
                consumerId, m_profileId, true,
                {QStringLiteral("inner_thought"), QStringLiteral("diary_entry")}));
    }
    return Result<EventReadAuthorization, DomainError>::failure(
        domainError(QStringLiteral("EVT_READ_FORBIDDEN"),
                    QStringLiteral("event consumer is not registered")));
}

void AgentRuntimeServices::stop() {
    if (!m_started && !m_eventSchemas && !m_eventRepository) return;
    if (m_aiBrain) m_aiBrain->setRuntimeServices(nullptr);
    m_eventOutbox.reset();
    m_unitOfWorkFactory.reset();
    m_checkpointStore.reset();
    m_eventLedger.reset();
    m_eventRepository.reset();
    m_eventSchemas.reset();
    m_uiBridge = nullptr;
    m_aiBrain = nullptr;
    m_profileId.clear();
    m_configHash.clear();
    m_identityBaselineHash.clear();
    m_started = false;
}
