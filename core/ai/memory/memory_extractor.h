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

    // Builds a bounded, personal Hippocampus impression for later Daydream
    // classification. Empty content means the input must not enter the inbox.
    MemoryEntry extractDaydreamImpression(const QString& input,
                                          const QString& triggerTag) const;
    static bool isLikelySensitiveContent(const QString& text);
};

#endif // DESKTOP_PET_MEMORY_EXTRACTOR_H
