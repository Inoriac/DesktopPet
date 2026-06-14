#ifndef DESKTOP_PET_MEMORY_EXTRACTOR_H
#define DESKTOP_PET_MEMORY_EXTRACTOR_H

#include <QList>
#include <QString>

#include "memory_types.h"

enum class MemoryCandidateOperation {
    Write,
    Forget
};

struct MemoryCandidate {
    MemoryCandidateOperation operation = MemoryCandidateOperation::Write;
    MemoryEntry entry;
    QString query;
    QString rawText;
    QString triggerTag;
    bool explicitRequest = false;
};

class MemoryExtractor {
public:
    QList<MemoryCandidate> extractFromUserInput(const QString& input,
                                                const QString& triggerTag) const;
};

#endif // DESKTOP_PET_MEMORY_EXTRACTOR_H
