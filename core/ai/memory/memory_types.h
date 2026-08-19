#ifndef DESKTOP_PET_MEMORY_TYPES_H
#define DESKTOP_PET_MEMORY_TYPES_H

#include <QDateTime>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include "emotion/emotion_types.h"

enum class MemoryType {
    Working,
    ShortTerm,
    Episodic,
    Semantic,
    Preference,
    Procedural,
    TaskShadow,
    Relationship,
    Core,
    Event
};

enum class MemoryStatus {
    Active,
    Archived,
    Superseded,
    Cancelled,
    Deleted,
    Expired
};

enum class PrivacyLevel {
    Public,
    Personal,
    Sensitive
};

inline QString memoryTypeToString(MemoryType type) {
    switch (type) {
    case MemoryType::Working:
        return "working";
    case MemoryType::ShortTerm:
        return "short_term";
    case MemoryType::Episodic:
        return "episodic";
    case MemoryType::Semantic:
        return "semantic";
    case MemoryType::Preference:
        return "preference";
    case MemoryType::Procedural:
        return "procedural";
    case MemoryType::TaskShadow:
        return "task_shadow";
    case MemoryType::Relationship:
        return "relationship";
    case MemoryType::Core:
        return "core";
    case MemoryType::Event:
        return "event";
    }
    return "event";
}

inline MemoryType memoryTypeFromString(const QString& value) {
    if (value == "working") return MemoryType::Working;
    if (value == "short_term") return MemoryType::ShortTerm;
    if (value == "episodic") return MemoryType::Episodic;
    if (value == "semantic") return MemoryType::Semantic;
    if (value == "preference") return MemoryType::Preference;
    if (value == "procedural") return MemoryType::Procedural;
    if (value == "task_shadow") return MemoryType::TaskShadow;
    if (value == "relationship") return MemoryType::Relationship;
    if (value == "core") return MemoryType::Core;
    return MemoryType::Event;
}

inline QString memoryStatusToString(MemoryStatus status) {
    switch (status) {
    case MemoryStatus::Active:
        return "active";
    case MemoryStatus::Archived:
        return "archived";
    case MemoryStatus::Superseded:
        return "superseded";
    case MemoryStatus::Cancelled:
        return "cancelled";
    case MemoryStatus::Deleted:
        return "deleted";
    case MemoryStatus::Expired:
        return "expired";
    }
    return "active";
}

inline MemoryStatus memoryStatusFromString(const QString& value) {
    if (value == "archived") return MemoryStatus::Archived;
    if (value == "superseded") return MemoryStatus::Superseded;
    if (value == "cancelled") return MemoryStatus::Cancelled;
    if (value == "deleted") return MemoryStatus::Deleted;
    if (value == "expired") return MemoryStatus::Expired;
    return MemoryStatus::Active;
}

inline QString privacyLevelToString(PrivacyLevel level) {
    switch (level) {
    case PrivacyLevel::Public:
        return "public";
    case PrivacyLevel::Personal:
        return "personal";
    case PrivacyLevel::Sensitive:
        return "sensitive";
    }
    return "public";
}

inline PrivacyLevel privacyLevelFromString(const QString& value) {
    if (value == "personal") return PrivacyLevel::Personal;
    if (value == "sensitive") return PrivacyLevel::Sensitive;
    return PrivacyLevel::Public;
}

struct MemoryEntry {
    QString id;
    MemoryType type = MemoryType::Event;
    MemoryStatus status = MemoryStatus::Active;
    PrivacyLevel privacyLevel = PrivacyLevel::Public;
    QString partition;   // 物理分区（hippocampus/episodic/semantic/preference/procedural），
                         // 派生自 type（见 partition_policy.h::partitionForType），持久化便于按分区扫描遗忘
    QString key;
    QJsonValue value;
    QString summary;
    QString content;
    QStringList tags;
    QString scope;
    QString source;
    double importance = 0.0;
    double strength = 0.0;
    double confidence = 0.0;
    EmotionType emotion = EmotionType::Neutral;
    double emotionIntensity = 0.0;
    double emotionConfidence = 0.0;
    int mentionCount = 0;
    int accessCount = 0;
    QDateTime createdAt;
    QDateTime updatedAt;
    QDateTime lastAccessedAt;
    QDateTime expiresAt;
    QStringList evidence;
    QStringList sourceMemoryIds;
    QStringList supersedes;
    QStringList conflictsWith;
    QJsonObject payload;

    QJsonObject toJson() const;
    static MemoryEntry fromJson(const QJsonObject& object);
};

#endif // DESKTOP_PET_MEMORY_TYPES_H
