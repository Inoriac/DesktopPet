#include "memory_json_codec.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

#include "memory_repository.h"

bool MemoryJsonCodec::exportSnapshot(const QList<MemoryEntry>& entries,
                                     const QString& filePath,
                                     QString* errorMessage) {
    const QFileInfo info(filePath);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().path())) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to create directory: %1").arg(info.dir().path());
        return false;
    }

    QJsonArray array;
    for (const MemoryEntry& entry : entries) {
        array.append(entry.toJson());
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }

    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    return true;
}

QList<MemoryEntry> MemoryJsonCodec::importSnapshot(const QString& filePath,
                                                    QString* errorMessage) {
    QList<MemoryEntry> entries;

    QFile file(filePath);
    if (!file.exists()) return entries;
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return entries;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (errorMessage) *errorMessage = parseError.errorString();
        return entries;
    }
    if (!doc.isArray()) {
        if (errorMessage) *errorMessage = QStringLiteral("JSON file is not an array");
        return entries;
    }

    for (const QJsonValue& value : doc.array()) {
        if (value.isObject()) {
            entries.append(MemoryEntry::fromJson(value.toObject()));
        }
    }
    return entries;
}

int MemoryJsonCodec::importLegacyJsonIfNeeded(const QString& jsonPath,
                                               MemoryRepository* repo,
                                               QString* errorMessage) {
    if (!repo || !repo->isOpen()) {
        if (errorMessage) *errorMessage = QStringLiteral("repository is not open");
        return -1;
    }
    if (!QFile::exists(jsonPath)) return 0;

    const QList<MemoryEntry> existing = repo->loadAll();
    QSet<QString> existingIds;
    QSet<QString> existingKeys;
    for (const MemoryEntry& entry : existing) {
        if (!entry.id.isEmpty()) existingIds.insert(entry.id);
        if (!entry.key.isEmpty()) existingKeys.insert(entry.key);
    }

    const QList<MemoryEntry> imported = importSnapshot(jsonPath, errorMessage);
    if (imported.isEmpty()) return 0;

    int count = 0;
    for (const MemoryEntry& entry : imported) {
        if (!entry.id.isEmpty() && existingIds.contains(entry.id)) continue;
        if (!entry.key.isEmpty() && existingKeys.contains(entry.key)) continue;

        if (repo->insert(entry)) {
            ++count;
            if (!entry.id.isEmpty()) existingIds.insert(entry.id);
            if (!entry.key.isEmpty()) existingKeys.insert(entry.key);
        }
    }

    return count;
}
