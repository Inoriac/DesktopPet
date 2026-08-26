#include "agent_runtime_services.h"

#include <QCryptographicHash>
#include <QDir>

#include "ai/ai_brain.h"
#include "ai/context/context_assembler.h"
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
#include "ai/reflection/daydream_sleep_adapter.h"
#include "ai/reflection/diary_service.h"
#include "ai/reflection/inner_thought_service.h"
#include "ai/reflection/private_key_provider.h"
#include "ai/reflection/private_psyche_crypto.h"
#include "ai/reflection/cancellation_token.h"
#include "ai/reflection/sleep_cycle_coordinator.h"
#include "ai/reflection/sleep_session_repository.h"
#include "ai/reflection/sqlite_private_psyche_repository.h"
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

    if (report.capabilities.profileGrowth
        && migration.profileStoreReady()
        && privateReflectionBuildAvailable()
        && request.agentScheduler) {
        const QString privateDatabasePath = QDir(request.profileMigration.appDataRoot)
            .filePath(QStringLiteral("profiles/%1/private_psyche.sqlite").arg(m_profileId));
        m_privateKeyProvider = std::make_unique<QtKeychainPrivateKeyProvider>();
        m_privateCrypto = std::make_unique<SodiumPrivatePsycheCrypto>();
        const auto key = m_privateKeyProvider->loadOrCreate(m_profileId);
        if (!key.isOk()) {
            report.diagnostics.append(key.error().message);
        } else {
            m_privateRepository = std::make_unique<SqlitePrivatePsycheRepository>();
            const auto privateOpened = m_privateRepository->open(privateDatabasePath);
            m_sleepSessionRepository = std::make_unique<SleepSessionRepository>();
            const auto sleepOpened = m_sleepSessionRepository->open(runtimeDatabasePath);
            if (!privateOpened.isOk() || !sleepOpened.isOk()) {
                report.diagnostics.append(
                    !privateOpened.isOk() ? privateOpened.error().message
                                          : sleepOpened.error().message);
            } else {
                m_agentScheduler = request.agentScheduler;
                m_reflectionContextAssembler = std::make_unique<ContextAssembler>();
                m_reflectionCancellation = std::make_unique<CancellationSource>();
                m_innerThoughtService = std::make_unique<InnerThoughtService>(
                    m_profileId, request.aiBrain->modelRouter(),
                    m_privateKeyProvider.get(), m_privateCrypto.get(),
                    m_privateRepository.get(), m_eventLedger.get());
                m_diaryService = std::make_unique<DiaryService>(
                    m_profileId, request.aiBrain->modelRouter(),
                    m_reflectionContextAssembler.get(), m_privateKeyProvider.get(),
                    m_privateCrypto.get(), m_privateRepository.get(),
                    ModelRole::Diary, m_eventLedger.get());
                m_daydreamSleepAdapter = std::make_unique<DaydreamSleepAdapter>(
                    m_profileId, request.profile.name, request.aiBrain->memoryStore(),
                    request.aiBrain->modelRouter());
                m_sleepCycleCoordinator = std::make_unique<SleepCycleCoordinator>(
                    m_profileId, request.sleepPolicy, m_sleepSessionRepository.get(),
                    m_daydreamSleepAdapter.get(), m_diaryService.get(),
                    m_privateRepository.get(), request.aiBrain,
                    request.agentScheduler, SleepCycleHooks{});
                m_sleepCycleCoordinator->start();
                if (m_sleepCycleCoordinator->isStarted()) {
                    report.capabilities.privateReflection = true;
                    report.capabilities.sleepCycle = true;
                } else {
                    report.diagnostics.append(
                        QStringLiteral("sleep recovery failed; private reflection disabled"));
                }
            }
        }
        if (!report.capabilities.sleepCycle) {
            m_sleepCycleCoordinator.reset();
            m_daydreamSleepAdapter.reset();
            m_diaryService.reset();
            m_innerThoughtService.reset();
            m_reflectionCancellation.reset();
            m_reflectionContextAssembler.reset();
            m_sleepSessionRepository.reset();
            m_privateRepository.reset();
            m_privateCrypto.reset();
            m_privateKeyProvider.reset();
            m_agentScheduler = nullptr;
        }
    } else if (report.capabilities.profileGrowth) {
        report.diagnostics.append(QStringLiteral(
            "private reflection dependencies are unavailable; legacy Daydream retained"));
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

void AgentRuntimeServices::reflectOnCompletedSession(const QString& sessionId) {
    if (!m_started || sessionId.trimmed().isEmpty() || !m_eventLedger
        || !m_innerThoughtService || !m_reflectionCancellation) {
        return;
    }
    const auto authorization = authorizationFor(QStringLiteral("reflection"));
    if (!authorization.isOk()) return;
    const EventFilter filter{
        {QStringLiteral("UserMessageReceived"),
         QStringLiteral("AssistantResponseProduced")},
        sessionId,
        authorization.value()
    };
    const auto events = m_eventLedger->readAfter(0, filter, 64);
    if (!events.isOk()) return;

    std::optional<EventRecord> userEvent;
    std::optional<EventRecord> assistantEvent;
    for (const EventRecord& event : events.value()) {
        if (event.type == QLatin1String("UserMessageReceived")) {
            userEvent = event;
        } else if (event.type == QLatin1String("AssistantResponseProduced")) {
            assistantEvent = event;
        }
    }
    if (!userEvent.has_value() || !assistantEvent.has_value()
        || assistantEvent->sequence <= userEvent->sequence) {
        return;
    }
    const QString triggerTag = userEvent->payload
                                   .value(QStringLiteral("triggerTag")).toString();
    if (triggerTag != QLatin1String("manual")
        && triggerTag != QLatin1String("user_request")
        && triggerTag != QLatin1String("touch_event")) {
        return;
    }
    const QString userText = userEvent->payload
                                 .value(QStringLiteral("text")).toString().simplified();
    const QString assistantText = assistantEvent->payload
                                      .value(QStringLiteral("text")).toString().simplified();
    if (userText.isEmpty() || assistantText.isEmpty()
        || (userText.size() < 8 && userText.size() + assistantText.size() < 80)) {
        return;
    }

    InnerThoughtRequest request;
    request.profileId = m_profileId;
    request.sourceEventId = assistantEvent->eventId;
    request.contextSnapshot = {
        {QStringLiteral("sessionId"), sessionId},
        {QStringLiteral("user"), userText.left(2000)},
        {QStringLiteral("assistant"), assistantText.left(2000)},
        {QStringLiteral("triggerTag"), triggerTag}
    };
    if (m_emotionStateProvider) {
        request.emotionSnapshot = m_emotionStateProvider->currentSnapshot(
            m_profileId, assistantEvent->occurredAt);
    }
    m_innerThoughtService->createAsync(
        request, m_reflectionCancellation->token(),
        [](Result<QString, DomainError>) {});
}

void AgentRuntimeServices::cancelSleepForUserInteraction() {
    if (m_sleepCycleCoordinator) {
        m_sleepCycleCoordinator->cancelActive(SleepCancelReason::UserInteraction);
    }
}

void AgentRuntimeServices::stop() {
    if (!m_started && !m_eventSchemas && !m_eventRepository) return;
    if (m_sleepCycleCoordinator) m_sleepCycleCoordinator->stop();
    if (m_reflectionCancellation) m_reflectionCancellation->cancel();
    m_sleepCycleCoordinator.reset();
    m_daydreamSleepAdapter.reset();
    m_diaryService.reset();
    m_innerThoughtService.reset();
    m_reflectionCancellation.reset();
    m_reflectionContextAssembler.reset();
    m_sleepSessionRepository.reset();
    m_privateRepository.reset();
    m_privateCrypto.reset();
    m_privateKeyProvider.reset();
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
    if (m_aiBrain) m_aiBrain->setRuntimeServices(nullptr);
    m_agentScheduler = nullptr;
    m_uiBridge = nullptr;
    m_aiBrain = nullptr;
    m_profileId.clear();
    m_configHash.clear();
    m_identityBaselineHash.clear();
    m_identityBaseline = IdentityBaseline::defaults();
    m_personalityPolicy = PersonalityPolicy{};
    m_started = false;
}
