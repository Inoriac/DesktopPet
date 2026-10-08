#ifndef DESKTOP_PET_MEMORY_CONSOLIDATION_SERVICE_H
#define DESKTOP_PET_MEMORY_CONSOLIDATION_SERVICE_H

#include <atomic>
#include <functional>
#include <memory>

#include "ai/memory/daydream_consolidator.h"
#include "cancellation_token.h"
#include "reflection_types.h"

class MemoryStore;
class ModelRouter;
class ChatSideEffectQueue;

class MemoryConsolidationService {
public:
    MemoryConsolidationService(QString profileId,
                         QString petName,
                         MemoryStore* memoryStore,
                         ModelRouter* modelRouter);
    ~MemoryConsolidationService();

    // One foreground-independent executor; SQLite work uses the chat writer.
    void maintainAsync(ChatSideEffectQueue* writer, int maxItems,
                       const CancellationToken& token,
                       std::function<void(QJsonObject)> completed,
                       int batchLimit = 8, int relatedLimit = 8);
    void setContext(QString profileId, QString petName);

    // Recovery only: honor historical sleep Commit/Abort decisions.
    Result<void, DomainError> finalizeSession(const QString& sessionId);
    Result<void, DomainError> abortSession(const QString& sessionId);
    int preparedChangeCount(const QString& sessionId) const;

private:
    struct ConsolidationState;
    void processNextBatch(const std::shared_ptr<ConsolidationState>& state);
    void analyzeBatch(const std::shared_ptr<ConsolidationState>& state,
                      const QList<MemoryEntry>& batch, const QList<MemoryEntry>& related);
    void completeBatch(const std::shared_ptr<ConsolidationState>& state,
                       const QList<MemoryEntry>& batch,
                       const QList<DaydreamDecision>& decisions,
                       const QList<RelationProposal>& proposals = {});

    QString m_profileId;
    QString m_petName;
    MemoryStore* m_memoryStore = nullptr;
    ModelRouter* m_modelRouter = nullptr;
    std::shared_ptr<std::atomic_bool> m_alive =
        std::make_shared<std::atomic_bool>(true);
};

#endif // DESKTOP_PET_MEMORY_CONSOLIDATION_SERVICE_H
