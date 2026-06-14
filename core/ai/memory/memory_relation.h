#ifndef DESKTOP_PET_MEMORY_RELATION_H
#define DESKTOP_PET_MEMORY_RELATION_H

#include <QDateTime>
#include <QJsonObject>
#include <QString>

enum class MemoryRelationType {
    Related,
    TopicOf,
    CreatedTask,
    Supersedes,
    ConflictsWith,
    DerivedFrom,
    MentionedWith
};

inline QString memoryRelationTypeToString(MemoryRelationType type) {
    switch (type) {
    case MemoryRelationType::Related:       return QStringLiteral("related");
    case MemoryRelationType::TopicOf:       return QStringLiteral("topic_of");
    case MemoryRelationType::CreatedTask:   return QStringLiteral("created_task");
    case MemoryRelationType::Supersedes:    return QStringLiteral("supersedes");
    case MemoryRelationType::ConflictsWith: return QStringLiteral("conflicts_with");
    case MemoryRelationType::DerivedFrom:   return QStringLiteral("derived_from");
    case MemoryRelationType::MentionedWith: return QStringLiteral("mentioned_with");
    }
    return QStringLiteral("related");
}

inline MemoryRelationType memoryRelationTypeFromString(const QString& value) {
    if (value == QLatin1String("topic_of"))       return MemoryRelationType::TopicOf;
    if (value == QLatin1String("created_task"))    return MemoryRelationType::CreatedTask;
    if (value == QLatin1String("supersedes"))      return MemoryRelationType::Supersedes;
    if (value == QLatin1String("conflicts_with"))  return MemoryRelationType::ConflictsWith;
    if (value == QLatin1String("derived_from"))    return MemoryRelationType::DerivedFrom;
    if (value == QLatin1String("mentioned_with"))  return MemoryRelationType::MentionedWith;
    return MemoryRelationType::Related;
}

struct MemoryRelation {
    QString id;
    QString fromMemoryId;
    QString toMemoryId;
    MemoryRelationType type = MemoryRelationType::Related;
    double weight = 1.0;
    double confidence = 1.0;
    QDateTime createdAt;
    QJsonObject payload;
};

#endif // DESKTOP_PET_MEMORY_RELATION_H
