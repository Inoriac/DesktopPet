#ifndef DESKTOP_PET_OWNER_DIARY_FACADE_H
#define DESKTOP_PET_OWNER_DIARY_FACADE_H

#include "ai/domain/domain_result.h"
#include "ai/reflection/reflection_types.h"

class DiaryService;

class OwnerDiaryFacade {
public:
    explicit OwnerDiaryFacade(DiaryService* diaryService);

    Result<DiaryPage, DomainError> list(
        const DiaryListQuery& query,
        const OwnerAuthContext& auth) const;
    Result<DiaryEntry, DomainError> get(
        const QString& entryId,
        const OwnerAuthContext& auth) const;

private:
    DiaryService* m_diaryService = nullptr;
};

#endif // DESKTOP_PET_OWNER_DIARY_FACADE_H
