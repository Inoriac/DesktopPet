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

// 边来源（设计 §10：规则、工具、Embedding、共现和模型来源必须可区分）
enum class RelationProvenance {
    Rule,          // 代码确定性规则（MemoryPolicy / Daydream 规则）
    Tool,          // 工具调用产生
    Embedding,     // HNSW/Embedding 相似候选
    Cooccurrence,  // 标签/事件共现
    Model,         // LLM 模型判断（复核确认）
    Legacy         // 迁移前的存量数据
};

inline QString relationProvenanceToString(RelationProvenance provenance) {
    switch (provenance) {
    case RelationProvenance::Rule:         return QStringLiteral("rule");
    case RelationProvenance::Tool:         return QStringLiteral("tool");
    case RelationProvenance::Embedding:    return QStringLiteral("embedding");
    case RelationProvenance::Cooccurrence: return QStringLiteral("cooccurrence");
    case RelationProvenance::Model:        return QStringLiteral("model");
    case RelationProvenance::Legacy:       return QStringLiteral("legacy");
    }
    return QStringLiteral("rule");
}

inline RelationProvenance relationProvenanceFromString(const QString& value) {
    if (value == QLatin1String("tool"))         return RelationProvenance::Tool;
    if (value == QLatin1String("embedding"))    return RelationProvenance::Embedding;
    if (value == QLatin1String("cooccurrence")) return RelationProvenance::Cooccurrence;
    if (value == QLatin1String("model"))        return RelationProvenance::Model;
    if (value == QLatin1String("legacy"))       return RelationProvenance::Legacy;
    return RelationProvenance::Rule;
}

// 结构性边（设计 §10：DerivedFrom/Supersedes/CreatedTask 与确认后的 ConflictsWith
// 不按普通共现衰减，也不受节点联想边上限约束）。
inline bool isStructuralRelation(MemoryRelationType type,
                                 RelationProvenance provenance = RelationProvenance::Rule) {
    switch (type) {
    case MemoryRelationType::DerivedFrom:
    case MemoryRelationType::Supersedes:
    case MemoryRelationType::CreatedTask:
        return true;
    case MemoryRelationType::ConflictsWith:
        return provenance == RelationProvenance::Model;  // 仅模型确认后的冲突边
    default:
        return false;
    }
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
    // Phase 4.2 新增（设计 §10）
    RelationProvenance provenance = RelationProvenance::Rule;
    int supportCount = 1;          // 被独立证据支持的次数（重复边合并时递增）
    QDateTime updatedAt;           // 最后一次属性变更
    QDateTime lastReinforcedAt;    // 最后一次被强化（重见/共现）

    bool structural() const { return isStructuralRelation(type, provenance); }
};

#endif // DESKTOP_PET_MEMORY_RELATION_H
