#ifndef DESKTOP_PET_AGENT_RUNTIME_SERVICES_H
#define DESKTOP_PET_AGENT_RUNTIME_SERVICES_H

#include <memory>

#include "ai/domain/domain_result.h"
#include "ai/event/event_types.h"
#include "runtime_types.h"

class AgentBootstrap;
class EventConsumerCheckpointStore;
class EventLedger;
class EmotionStateProvider;
class EventOutbox;
class EventSchemaRegistry;
class NullEmotionStateProvider;
class PersonaProjector;
struct PersonaProjection;
class PersonalityService;
class RelationshipService;
class RuntimeUnitOfWorkFactory;
class SelfModelService;
class SqliteEventRepository;
class SqliteIdentityRepository;

class AgentRuntimeServices {
public:
    AgentRuntimeServices() = default;
    ~AgentRuntimeServices();

    AgentRuntimeServices(const AgentRuntimeServices&) = delete;
    AgentRuntimeServices& operator=(const AgentRuntimeServices&) = delete;

    RuntimeSnapshot captureSnapshot(
        const QString& sessionId,
        const QString& subjectId = QStringLiteral("owner")) const;
    PersonaProjection projectPersona(
        const RuntimeSnapshot& snapshot,
        const InteractionContext& context) const;
    Result<EventReadAuthorization, DomainError> authorizationFor(
        const QString& consumerId) const;
    EventLedger* eventLedger() const { return m_eventLedger.get(); }
    EventOutbox* eventOutbox() const { return m_eventOutbox.get(); }
    RuntimeUnitOfWorkFactory* unitOfWorkFactory() const {
        return m_unitOfWorkFactory.get();
    }
    EventConsumerCheckpointStore* checkpointStore() const {
        return m_checkpointStore.get();
    }
    PersonalityService* personalityService() const { return m_personalityService.get(); }
    RelationshipService* relationshipService() const { return m_relationshipService.get(); }
    SelfModelService* selfModelService() const { return m_selfModelService.get(); }
    bool isStarted() const { return m_started; }
    void stop();

private:
    friend class AgentBootstrap;
    Result<RuntimeStartReport, DomainError> startAfterStorageReady(
        const RuntimeStartRequest& request,
        const ProfileMigrationResult& migration);

    QString m_profileId;
    QString m_configHash;
    int m_identityBaselineSchemaVersion = 1;
    QString m_identityBaselineHash;
    IdentityBaseline m_identityBaseline = IdentityBaseline::defaults();
    PersonalityPolicy m_personalityPolicy;
    EmotionStateProvider* m_emotionStateProvider = nullptr;
    AIBrain* m_aiBrain = nullptr;
    RuntimeUiBridge* m_uiBridge = nullptr;
    bool m_started = false;

    std::unique_ptr<EventSchemaRegistry> m_eventSchemas;
    std::unique_ptr<SqliteEventRepository> m_eventRepository;
    std::unique_ptr<EventLedger> m_eventLedger;
    std::unique_ptr<EventConsumerCheckpointStore> m_checkpointStore;
    std::unique_ptr<RuntimeUnitOfWorkFactory> m_unitOfWorkFactory;
    std::unique_ptr<EventOutbox> m_eventOutbox;
    std::unique_ptr<NullEmotionStateProvider> m_nullEmotionStateProvider;
    std::unique_ptr<SqliteIdentityRepository> m_identityRepository;
    std::unique_ptr<PersonalityService> m_personalityService;
    std::unique_ptr<RelationshipService> m_relationshipService;
    std::unique_ptr<SelfModelService> m_selfModelService;
    std::unique_ptr<PersonaProjector> m_personaProjector;
};

#endif // DESKTOP_PET_AGENT_RUNTIME_SERVICES_H
