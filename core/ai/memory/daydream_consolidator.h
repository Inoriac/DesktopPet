#ifndef DESKTOP_PET_DAYDREAM_CONSOLIDATOR_H
#define DESKTOP_PET_DAYDREAM_CONSOLIDATOR_H

#include <QList>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "ai/domain/domain_result.h"
#include "memory_types.h"

class MemoryStore;

enum class DaydreamAction {
    Preserve,
    Create,
    Update,
    KeepBoth,
    Discard
};

struct DaydreamSnapshot {
    QList<MemoryEntry> items;

    bool isEmpty() const { return items.isEmpty(); }
    int size() const { return items.size(); }
};

struct DaydreamDecision {
    QString sourceId;
    DaydreamAction action = DaydreamAction::Preserve;
    MemoryType targetType = MemoryType::Episodic;
    QString targetMemoryId;
    QString mergedContent;
    double qualityScore = 0.0;
    QStringList tags;
    MemoryEntry expectedTarget;
};

struct DaydreamStats {
    int scanned = 0;
    int upgraded = 0;
    int updated = 0;
    int discarded = 0;
    int preserved = 0;
    int failed = 0;
    bool staleSnapshot = false;
    bool committed = false;
};

struct DaydreamChangeSet {
    QString changeSetId;
    DaydreamSnapshot snapshot;
    QList<DaydreamDecision> decisions;

    QJsonObject toJson() const;
    QString payloadHash() const;
    static Result<DaydreamChangeSet, DomainError> fromJson(
        const QJsonObject& object);
};

// Daydream uses snapshot -> decide -> short atomic commit. No SQLite transaction
// remains open while an asynchronous LLM request is in flight.
class DaydreamConsolidator {
public:
    static constexpr int SESSION_LIMIT = 32;
    static constexpr int BATCH_LIMIT = 8;
    static constexpr int INBOX_LIMIT = 200;

    using Action = DaydreamAction;
    using Snapshot = DaydreamSnapshot;
    using Decision = DaydreamDecision;
    using Stats = DaydreamStats;

    explicit DaydreamConsolidator(MemoryStore& store);

    int pendingCount() const;
    Snapshot createSnapshot(int maxItems = SESSION_LIMIT) const;
    QList<MemoryEntry> relatedLongTermMemories(const QList<MemoryEntry>& batch,
                                                int limit = 8) const;

    // Parse one complete LLM batch. The result must contain exactly one valid,
    // uniquely identified decision for every source in the batch.
    static bool parseDecisions(const QString& response,
                               const QList<MemoryEntry>& batch,
                               const QList<MemoryEntry>& allowedUpdateTargets,
                               QList<Decision>* decisions,
                               QString* errorMessage = nullptr);
    static bool requiresModelDecision(const MemoryEntry& entry);
    static QList<Decision> hardcodedDecisions(const QList<MemoryEntry>& batch);

    Result<DaydreamChangeSet, DomainError> buildChangeSet(
        const Snapshot& snapshot,
        const QList<Decision>& decisions) const;
    Stats applyChangeSet(const DaydreamChangeSet& changeSet);

    // Applies a fully staged session in one short transaction. If any source was
    // changed after snapshot creation, nothing is written and staleSnapshot=true.
    Stats applyDecisions(const Snapshot& snapshot,
                         const QList<Decision>& decisions);

    // Synchronous fallback used when the LLM is unavailable.
    Stats runHardcodedDrain(int maxItems = SESSION_LIMIT);

private:
    bool snapshotStillCurrent(const Snapshot& snapshot) const;
    bool updateTargetsStillCurrent(const QList<Decision>& decisions) const;
    bool applyOne(const MemoryEntry& source,
                  const Decision& decision,
                  Stats* stats);
    MemoryEntry makeLongTermEntry(const MemoryEntry& source,
                                  const Decision& decision) const;

    MemoryStore& m_store;
};

#endif // DESKTOP_PET_DAYDREAM_CONSOLIDATOR_H
