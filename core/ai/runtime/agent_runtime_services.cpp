#include "agent_runtime_services.h"

#include <QCryptographicHash>
#include <QDir>

#include "ai/ai_brain.h"
#include "ai/event/event_ledger.h"
#include "ai/event/event_outbox.h"
#include "ai/event/event_schema_registry.h"
#include "ai/event/runtime_unit_of_work.h"
#include "ai/event/sqlite_event_repository.h"
#include "ai/identity/persona_projector.h"
#include "ai/identity/personality_service.h"
#include "ai/identity/relationship_service.h"
#include "ai/identity/self_model_service.h"
#include "ai/identity/sqlite_identity_repository.h"
#include "ai/integration/emotion_state_provider.h"
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
    m_identityBaseline = request.identityBaseline;
    m_personalityPolicy = sanitizePersonalityPolicy(request.personalityPolicy);
    m_aiBrain = request.aiBrain;
    m_uiBridge = request.uiBridge;
    m_started = true;

    RuntimeStartReport report;
    report.profileMigration = migration;
    report.capabilities.profileStore = migration.profileStoreReady();
    report.capabilities.profileGrowth = false;
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

    if (migration.profileStoreReady()) {
        m_nullEmotionStateProvider = std::make_unique<NullEmotionStateProvider>();
        m_emotionStateProvider = request.emotionStateProvider
            ? request.emotionStateProvider : m_nullEmotionStateProvider.get();
        m_identityRepository = std::make_unique<SqliteIdentityRepository>();
        const Result<void, DomainError> identityOpened =
            m_identityRepository->open(runtimeDatabasePath);
        if (identityOpened.isOk()) {
            m_personalityService = std::make_unique<PersonalityService>(
                m_profileId,
                m_identityBaseline,
                m_personalityPolicy,
                m_identityRepository.get(),
                m_unitOfWorkFactory.get(),
                m_eventOutbox.get());
            m_relationshipService = std::make_unique<RelationshipService>(
                m_profileId, m_identityRepository.get());
            m_selfModelService = std::make_unique<SelfModelService>(
                m_profileId, m_identityRepository.get());
            m_personaProjector = std::make_unique<PersonaProjector>(
                m_identityBaseline,
                m_personalityPolicy,
                m_identityRepository.get(),
                m_emotionStateProvider);
            report.capabilities.profileGrowth = true;
        } else {
            report.diagnostics.append(identityOpened.error().message);
            m_identityRepository.reset();
            m_nullEmotionStateProvider.reset();
            m_emotionStateProvider = nullptr;
        }
    }

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

RuntimeSnapshot AgentRuntimeServices::captureSnapshot(
    const QString& sessionId, const QString& subjectId) const {
    if (!m_started || sessionId.trimmed().isEmpty()) return {};
    RuntimeSnapshot snapshot;
    snapshot.sessionId = sessionId;
    snapshot.profileId = m_profileId;
    snapshot.subjectId = subjectId.trimmed().isEmpty()
        ? QStringLiteral("owner") : subjectId.trimmed().left(128);
    snapshot.identityBaselineSchemaVersion = m_identityBaselineSchemaVersion;
    snapshot.identityBaselineHash = m_identityBaselineHash;
    snapshot.configHash = m_configHash;
    snapshot.capturedAt = QDateTime::currentDateTimeUtc();
    if (m_identityRepository) {
        const auto personality = m_identityRepository->currentPersonality(m_profileId);
        if (personality.isOk() && personality.value().has_value()) {
            snapshot.personalityVersion = personality.value()->version;
        }
        const auto relationship = m_identityRepository->currentRelationship(
            m_profileId, snapshot.subjectId);
        if (relationship.isOk() && relationship.value().has_value()) {
            snapshot.relationshipVersion = relationship.value()->version;
        }
        const auto selfModel = m_identityRepository->currentSelfModel(m_profileId);
        if (selfModel.isOk() && selfModel.value().has_value()) {
            snapshot.selfModelVersion = selfModel.value()->versionId;
        }
    }
    return snapshot;
}

PersonaProjection AgentRuntimeServices::projectPersona(
    const RuntimeSnapshot& snapshot,
    const InteractionContext& context) const {
    if (m_personaProjector && snapshot.profileId == m_profileId) {
        return m_personaProjector->project(snapshot, context);
    }
    return PersonaProjector().projectBaseline(m_identityBaseline, context.petName);
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
    m_personaProjector.reset();
    m_selfModelService.reset();
    m_relationshipService.reset();
    m_personalityService.reset();
    m_identityRepository.reset();
    m_nullEmotionStateProvider.reset();
    m_emotionStateProvider = nullptr;
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
    m_identityBaseline = IdentityBaseline::defaults();
    m_personalityPolicy = PersonalityPolicy{};
    m_started = false;
}
