#ifndef DESKTOP_PET_MEMORY_POLICY_H
#define DESKTOP_PET_MEMORY_POLICY_H

#include <QList>
#include <QStringList>

#include "memory_extractor.h"
#include "memory_relation.h"
#include "memory_store.h"

struct MemoryPolicyReport {
    int written = 0;
    int forgotten = 0;
    int skipped = 0;
    int relationsCreated = 0;
    QStringList notes;
};

struct StagedMemoryPolicyResult {
    MemoryPolicyReport report;
    MemoryMutationBatch mutations;
};

class MemoryPolicy {
public:
    static bool matchesForgetQuery(const MemoryEntry& entry,
                                   const QString& query);

    MemoryPolicyReport applyCandidates(const QList<MemoryCandidate>& candidates,
                                       MemoryStore* store) const;
    StagedMemoryPolicyResult stageCandidates(
        const QList<MemoryCandidate>& candidates,
        MemoryStore* store) const;

private:
    bool shouldAutoWrite(const MemoryCandidate& candidate, QString* reason) const;
    void discoverRelations(const MemoryEntry& newEntry,
                           MemoryStore* store,
                           MemoryPolicyReport* report,
                           MemoryMutationBatch* mutations) const;
    void discoverMentionedWith(const QList<MemoryEntry>& writtenEntries,
                               MemoryPolicyReport* report,
                               MemoryMutationBatch* mutations) const;
};

#endif // DESKTOP_PET_MEMORY_POLICY_H
