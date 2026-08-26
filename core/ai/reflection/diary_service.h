#ifndef DESKTOP_PET_DIARY_SERVICE_H
#define DESKTOP_PET_DIARY_SERVICE_H

#include "cancellation_token.h"
#include "reflection_types.h"

#include <atomic>
#include <memory>

class ContextAssembler;
class EventLedger;
class ModelRouter;
class PrivateKeyProvider;
class PrivatePsycheCrypto;
class SqlitePrivatePsycheRepository;

class DiaryService {
public:
    DiaryService(QString profileId,
                 ModelRouter* modelRouter,
                 ContextAssembler* contextAssembler,
                 PrivateKeyProvider* keyProvider,
                 PrivatePsycheCrypto* crypto,
                 SqlitePrivatePsycheRepository* repository,
                 ModelRole selfReadRole = ModelRole::Diary,
                 EventLedger* eventLedger = nullptr);
    ~DiaryService();

    void composeAsync(const DiaryRequest& request,
                      StagingSession& staging,
                      const CancellationToken& token,
                      DiaryHandler handler);
    Result<DiaryEntry, DomainError> readForSelf(const QString& entryId) const;
    Result<DiaryPage, DomainError> listForOwner(
        const DiaryListQuery& query,
        const OwnerAuthContext& auth) const;
    Result<DiaryEntry, DomainError> readForOwner(
        const QString& entryId,
        const OwnerAuthContext& auth) const;
    Result<bool, DomainError> hasCommittedDiaryForDate(
        const QDate& localDate) const;
    Result<void, DomainError> finalizeSession(const QString& sessionId);
    Result<void, DomainError> abortSession(const QString& sessionId);

private:
    Result<DiaryEntry, DomainError> readEntry(const QString& entryId) const;

    QString m_profileId;
    ModelRouter* m_modelRouter = nullptr;
    ContextAssembler* m_contextAssembler = nullptr;
    PrivateKeyProvider* m_keyProvider = nullptr;
    PrivatePsycheCrypto* m_crypto = nullptr;
    SqlitePrivatePsycheRepository* m_repository = nullptr;
    ModelRole m_selfReadRole = ModelRole::Diary;
    EventLedger* m_eventLedger = nullptr;
    std::shared_ptr<std::atomic_bool> m_alive =
        std::make_shared<std::atomic_bool>(true);
};

#endif // DESKTOP_PET_DIARY_SERVICE_H
