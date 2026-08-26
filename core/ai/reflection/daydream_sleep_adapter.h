#ifndef DESKTOP_PET_DAYDREAM_SLEEP_ADAPTER_H
#define DESKTOP_PET_DAYDREAM_SLEEP_ADAPTER_H

#include <atomic>
#include <functional>
#include <memory>

#include "ai/memory/daydream_consolidator.h"
#include "cancellation_token.h"
#include "reflection_types.h"

class MemoryStore;
class ModelRouter;

using DaydreamCompletionHandler =
    std::function<void(Result<DaydreamChangeSet, DomainError>)>;

class DaydreamSleepAdapter {
public:
    DaydreamSleepAdapter(QString profileId,
                         QString petName,
                         MemoryStore* memoryStore,
                         ModelRouter* modelRouter);
    ~DaydreamSleepAdapter();

    void consolidateAsync(const DaydreamRequest& request,
                          StagingSession& staging,
                          const CancellationToken& token,
                          DaydreamCompletionHandler handler);
    Result<void, DomainError> finalizeSession(const QString& sessionId);
    Result<void, DomainError> abortSession(const QString& sessionId);
    int preparedChangeCount(const QString& sessionId) const;

private:
    struct ConsolidationState;
    void processNextBatch(const std::shared_ptr<ConsolidationState>& state);
    void finishStaging(const std::shared_ptr<ConsolidationState>& state);

    QString m_profileId;
    QString m_petName;
    MemoryStore* m_memoryStore = nullptr;
    ModelRouter* m_modelRouter = nullptr;
    std::shared_ptr<std::atomic_bool> m_alive =
        std::make_shared<std::atomic_bool>(true);
};

#endif // DESKTOP_PET_DAYDREAM_SLEEP_ADAPTER_H
