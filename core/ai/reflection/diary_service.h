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
class DiaryFragmentService;

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

    // 可选：注入片段服务后，composeAsync 会把当日 Draft 便签缝合进日记，
    // finalizeSession 成功后把被缝合的片段标记为 Consumed。
    void setFragmentService(DiaryFragmentService* fragmentService) {
        m_fragmentService = fragmentService;
    }

    void composeAsync(const DiaryRequest& request,
                      StagingSession& staging,
                      const CancellationToken& token,
                      DiaryHandler handler);
    // 补写：对存在孤儿便签的历史日期独立缝合一篇日记（recovery 人格化叙事）。
    // 自管理 staging session（"diary-recovery-<date>"），成功即 finalize，失败 abort。
    void composeRecoveryAsync(const QDate& localDate, DiaryHandler handler = {});
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
    DiaryFragmentService* m_fragmentService = nullptr;
    std::shared_ptr<std::atomic_bool> m_alive =
        std::make_shared<std::atomic_bool>(true);
};

#endif // DESKTOP_PET_DIARY_SERVICE_H
