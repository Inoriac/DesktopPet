#ifndef DESKTOP_PET_MEMORY_ATTRIBUTION_SERVICE_H
#define DESKTOP_PET_MEMORY_ATTRIBUTION_SERVICE_H
#include "memory_extractor.h"
#include "memory_retriever.h"
#include "memory_store.h"
#include "working_memory_cache.h"
#include <functional>
#include <optional>
struct MemoryOriginSource {
    QString profileId, originId, speakerPersonId, conversationId, text;
    QDateTime occurredAt;
    OriginKind originKind = OriginKind::Internal;
    QList<MemoryPersonReference> resolvedPeople;
};
struct AttributedMemoryInput {
    MemoryEntry entry;
    AttributionContext attribution;
    QList<MemoryPersonReference> describedPeople;
    bool explicitRequest = false;
};
struct MemoryWriteResult {
    QStringList memoryIds;
    int written = 0, merged = 0, unresolved = 0;
};
struct ScopedMemoryQuery {
    QString text;
    QStringList subjectPersonIds;
    ContextSpec context;
    int limit = 8;
};
struct AttributionMigrationRequest {
    QString profileId, cursor;
    int batchSize = 100;
};
struct MigrationProgress {
    QString nextCursor;
    int processed = 0, unresolved = 0;
    bool complete = false;
    bool deferredForRecovery = false;
};
class MemoryAttributionService {
public:
    using SourceLookup = std::function<Result<std::optional<MemoryOriginSource>, DomainError>(
        const QString&, const QString&)>;
    using PersonLookup = std::function<Result<bool, DomainError>(const QString&, const QString&)>;
    MemoryAttributionService(QString profileId, MemoryStore* store, SourceLookup sourceLookup = {},
                             PersonLookup personLookup = {});
    Result<MemoryWriteResult, DomainError> ingestMemory(const AttributedMemoryInput& input);
    Result<QList<RetrievedMemory>, DomainError> recallForContext(const ScopedMemoryQuery& query);
    Result<MigrationProgress, DomainError>
    migrateAttribution(const AttributionMigrationRequest& request);
    Result<MemoryEntry, DomainError> attribute(const AttributedMemoryInput& input) const;

private:
    QString m_profileId;
    MemoryStore* m_store;
    SourceLookup m_sourceLookup;
    PersonLookup m_personLookup;
};
#endif
