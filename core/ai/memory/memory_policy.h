#ifndef DESKTOP_PET_MEMORY_POLICY_H
#define DESKTOP_PET_MEMORY_POLICY_H

#include <QList>
#include <QStringList>

#include "memory_extractor.h"

class MemoryStore;

struct MemoryPolicyReport {
    int written = 0;
    int forgotten = 0;
    int skipped = 0;
    QStringList notes;
};

class MemoryPolicy {
public:
    MemoryPolicyReport applyCandidates(const QList<MemoryCandidate>& candidates,
                                       MemoryStore* store) const;

private:
    bool shouldAutoWrite(const MemoryCandidate& candidate, QString* reason) const;
};

#endif // DESKTOP_PET_MEMORY_POLICY_H
