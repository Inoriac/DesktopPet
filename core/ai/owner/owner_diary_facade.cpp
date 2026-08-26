#include "owner_diary_facade.h"

#include "ai/reflection/diary_service.h"

namespace {

DomainError genericNotFound() {
    return domainError(QStringLiteral("DIARY_ENTRY_NOT_FOUND"),
                       QStringLiteral("diary entry is unavailable"));
}

bool shouldHide(const DomainError& error) {
    return error.code == QLatin1String("OWNER_AUTH_FAILED")
        || error.code == QLatin1String("CONTEXT_SCOPE_DENIED")
        || error.code == QLatin1String("PRIVATE_AUTH_FAILED");
}

} // namespace

OwnerDiaryFacade::OwnerDiaryFacade(DiaryService* diaryService)
    : m_diaryService(diaryService) {}

Result<DiaryPage, DomainError> OwnerDiaryFacade::list(
    const DiaryListQuery& query,
    const OwnerAuthContext& auth) const {
    if (!m_diaryService) {
        return Result<DiaryPage, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                        QStringLiteral("owner diary service is unavailable")));
    }
    const auto result = m_diaryService->listForOwner(query, auth);
    if (!result.isOk() && shouldHide(result.error())) {
        return Result<DiaryPage, DomainError>::failure(genericNotFound());
    }
    return result;
}

Result<DiaryEntry, DomainError> OwnerDiaryFacade::get(
    const QString& entryId,
    const OwnerAuthContext& auth) const {
    if (!m_diaryService || entryId.trimmed().isEmpty()) {
        return Result<DiaryEntry, DomainError>::failure(genericNotFound());
    }
    const auto result = m_diaryService->readForOwner(entryId, auth);
    if (!result.isOk() && shouldHide(result.error())) {
        return Result<DiaryEntry, DomainError>::failure(genericNotFound());
    }
    return result;
}
