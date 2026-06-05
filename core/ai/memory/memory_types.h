#ifndef DESKTOP_PET_MEMORY_TYPES_H
#define DESKTOP_PET_MEMORY_TYPES_H

#include <QDateTime>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

enum class MemoryType {
    ShortTerm,
    Preference,
    Event
};

inline QString memoryTypeToString(MemoryType type) {
    switch (type) {
    case MemoryType::ShortTerm:
        return "short_term";
    case MemoryType::Preference:
        return "preference";
    case MemoryType::Event:
        return "event";
    }
    return "event";
}

inline MemoryType memoryTypeFromString(const QString& value) {
    if (value == "short_term") return MemoryType::ShortTerm;
    if (value == "preference") return MemoryType::Preference;
    return MemoryType::Event;
}

struct MemoryEntry {
    QString id;
    MemoryType type = MemoryType::Event;
    QString key;
    QJsonValue value;
    QStringList tags;
    QDateTime createdAt;
    QDateTime updatedAt;

    QJsonObject toJson() const;
    static MemoryEntry fromJson(const QJsonObject& object);
};

#endif // DESKTOP_PET_MEMORY_TYPES_H