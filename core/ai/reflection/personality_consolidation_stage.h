#ifndef DESKTOP_PET_PERSONALITY_CONSOLIDATION_STAGE_H
#define DESKTOP_PET_PERSONALITY_CONSOLIDATION_STAGE_H

#include <atomic>
#include <functional>
#include <memory>

#include "ai/identity/identity_types.h"
#include "cancellation_token.h"
#include "reflection_types.h"

class ContextAssembler;
class ModelRouter;
class PersonalityService;
class SqliteIdentityRepository;

// Readers retain their ordinary authorization/encryption boundaries. Private
// content exists only in the model context, never in identity evidence or logs.
struct PersonalityEvolutionHooks {
    std::function<Result<QList<EventRecord>, DomainError>(qint64)> readCommittedEvents;
    std::function<Result<DiaryEntry, DomainError>(const QString&)> readDiary;
    std::function<Result<InnerThoughtSummary, DomainError>(const QString&)> readInnerThought;
};

struct PersonalityEvolutionRequest {
    QString profileId;
    QString sessionId;
    qint64 sourceCutoffSequence = 0;
    QDateTime now;
};

using PersonalityEvolutionHandler =
    std::function<void(Result<PersonalitySnapshot, DomainError>)>;

class PersonalityConsolidationStage {
public:
    PersonalityConsolidationStage(QString profileId,
                                 PersonalityService* personality,
                                 SqliteIdentityRepository* identity,
                                 ModelRouter* router,
                                 ContextAssembler* assembler,
                                 PersonalityEvolutionHooks hooks);
    virtual ~PersonalityConsolidationStage();

    virtual void consolidateAsync(const PersonalityEvolutionRequest& request,
                                  const CancellationToken& token,
                                  PersonalityEvolutionHandler handler);

private:
    struct Attempt;
    void extract(const std::shared_ptr<Attempt>& attempt);
    void summarize(const std::shared_ptr<Attempt>& attempt,
                   const PersonalityConsolidationPreview& preview);
    void publish(const std::shared_ptr<Attempt>& attempt,
                 const PersonalityConsolidationPreview& preview,
                 const QString& behavior,
                 const QString& narrative);

    QString m_profileId;
    PersonalityService* m_personality = nullptr;
    SqliteIdentityRepository* m_identity = nullptr;
    ModelRouter* m_router = nullptr;
    ContextAssembler* m_assembler = nullptr;
    PersonalityEvolutionHooks m_hooks;
    std::shared_ptr<std::atomic_bool> m_alive =
        std::make_shared<std::atomic_bool>(true);
};

#endif // DESKTOP_PET_PERSONALITY_CONSOLIDATION_STAGE_H
