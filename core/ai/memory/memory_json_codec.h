#ifndef DESKTOP_PET_MEMORY_JSON_CODEC_H
#define DESKTOP_PET_MEMORY_JSON_CODEC_H

#include <QList>
#include <QString>

#include "memory_types.h"

class MemoryRepository;

class MemoryJsonCodec {
public:
    static bool exportSnapshot(const QList<MemoryEntry>& entries,
                               const QString& filePath,
                               QString* errorMessage = nullptr);

    static QList<MemoryEntry> importSnapshot(const QString& filePath,
                                             QString* errorMessage = nullptr);

    static int importLegacyJsonIfNeeded(const QString& jsonPath,
                                        MemoryRepository* repo,
                                        QString* errorMessage = nullptr);
};

#endif // DESKTOP_PET_MEMORY_JSON_CODEC_H
