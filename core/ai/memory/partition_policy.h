#ifndef DESKTOP_PET_PARTITION_POLICY_H
#define DESKTOP_PET_PARTITION_POLICY_H

#include <algorithm>
#include <cmath>

#include <QList>
#include <QString>

#include "memory_types.h"

// 记忆分区 —— 对齐 hebb-mind 的 5 个皮质分区（constants.py）。
// 设计取舍：不设独立 Core/永不遗忘分区 —— 自适应遗忘本身已让高 importance/高 access
// 的记忆近乎不朽（importance×access 拉伸有效半衰期）。Core 类型（用户身份等承载性事实）
// 落入 Semantic 并在写入时设 importance 下限，靠自适应保证近不朽，同时仍可被更高优先级
// 的事实 supersedes。性格/偏好偏移落 Preference（可演化：强化则固化为性格、不强化则衰减回归 JSON 基线）。
enum class MemoryPartition {
    Hippocampus,   // 工作记忆收件箱 / 待巩固区，不进遗忘扫描（由 Daydream 巩固清空）
    Episodic,      // 经历、事件、上下文交互
    Semantic,      // 事实、知识、世界信息（含 Core 类型身份事实）
    Preference,    // 用户偏好、好恶、性格偏移（JSON 基线之上的潜移默化 delta）
    Procedural     // 技能、操作方法、习得动作
};

inline QString partitionToString(MemoryPartition p) {
    switch (p) {
    case MemoryPartition::Hippocampus: return QStringLiteral("hippocampus");
    case MemoryPartition::Episodic:    return QStringLiteral("episodic");
    case MemoryPartition::Semantic:    return QStringLiteral("semantic");
    case MemoryPartition::Preference:  return QStringLiteral("preference");
    case MemoryPartition::Procedural:  return QStringLiteral("procedural");
    }
    return QStringLiteral("episodic");
}

inline MemoryPartition partitionFromString(const QString& value) {
    const QString v = value.trimmed().toLower();
    if (v == QLatin1String("hippocampus")) return MemoryPartition::Hippocampus;
    if (v == QLatin1String("semantic"))   return MemoryPartition::Semantic;
    if (v == QLatin1String("preference")) return MemoryPartition::Preference;
    if (v == QLatin1String("procedural")) return MemoryPartition::Procedural;
    // 兼容旧库残留的 "core" 值：Core 分区已撤销，并入 Semantic。
    if (v == QLatin1String("core"))       return MemoryPartition::Semantic;
    return MemoryPartition::Episodic;
}

// 10 种 MemoryType → 物理分区映射（memory_improvement_plan.md B3）。
// Working/ShortTerm/TaskShadow 进 Hippocampus 待 Daydream 巩固；Core 类型并入 Semantic。
inline MemoryPartition partitionForType(MemoryType type) {
    switch (type) {
    case MemoryType::Core:        return MemoryPartition::Semantic;   // 身份事实 → Semantic，靠自适应近不朽
    case MemoryType::Preference:  return MemoryPartition::Preference;
    case MemoryType::Procedural:  return MemoryPartition::Procedural;
    case MemoryType::Semantic:    return MemoryPartition::Semantic;
    case MemoryType::Episodic:
    case MemoryType::Event:       return MemoryPartition::Episodic;
    case MemoryType::Working:
    case MemoryType::ShortTerm:
    case MemoryType::TaskShadow:  return MemoryPartition::Hippocampus;
    case MemoryType::Relationship:return MemoryPartition::Semantic;
    }
    return MemoryPartition::Episodic;
}

// 自适应遗忘参数（对齐 hebb-mind REGION_FORGET_DEFAULTS：threshold 全 0.3，k_access 全 1.5）。
// model: eff_half_life = base × (1 + k_importance·(I/10) + k_access·(access/10))
//        retention(idle_days) = exp(−idle / eff_half_life)
//        forget when retention < threshold  ⇔  idle > eff · ln(1/threshold)
struct PartitionDecayPolicy {
    MemoryPartition partition;
    double baseHalfLifeDays;     // 基础半衰期（天）；< 0 表示不进遗忘扫描
    double kImportance;          // 重要性线性权重（importance∈[0,10]）
    double kAccess;              // 访问频率线性权重（access 不截断）
    double threshold;            // 留存率低于此值触发遗忘
    bool sweepEnabled;           // false = 该分区不清扫

    // 有效半衰期（天）。importance=0 仅不增益，不是删除信号。
    double effectiveHalfLife(double importance, int accessCount) const {
        if (baseHalfLifeDays < 0.0) return -1.0;
        return baseHalfLifeDays * (1.0
                                  + kImportance * (importance / 10.0)
                                  + kAccess * (accessCount / 10.0));
    }

    // 留存率 ∈ [0,1]。eff_halflife ≤ 0（不可遗忘分区）恒返回 1。
    double retention(double importance, int accessCount, double idleDays) const {
        if (!sweepEnabled || baseHalfLifeDays < 0.0) return 1.0;
        const double eff = effectiveHalfLife(importance, accessCount);
        if (eff <= 0.0) return 1.0;
        return std::exp(-std::max(idleDays, 0.0) / eff);
    }

    // 闲置多少天后遗忘。floored at minRetentionDays。
    double forgetIdleDays(double importance, int accessCount,
                          double minRetentionDays = 1.0) const {
        if (!sweepEnabled || baseHalfLifeDays < 0.0) return -1.0;  // 永不遗忘
        const double eff = effectiveHalfLife(importance, accessCount);
        return std::max(eff * std::log(1.0 / threshold), minRetentionDays);
    }
};

// 默认策略表（base_half_life 单位：天）。Hippocampus 不清扫（由 Daydream 巩固清空）。
// 来源对齐 hebb-mind/scheduler/forgetting_job.py:36-41 的 REGION_FORGET_DEFAULTS。
// 不设 Core 永不遗忘分区 —— 重要身份事实落入 Semantic，靠自适应近不朽。
inline QList<PartitionDecayPolicy> defaultPartitionPolicies() {
    return {
        {MemoryPartition::Hippocampus, -1.0, 0.0, 0.0, 1.0, false},
        {MemoryPartition::Episodic,     30.0, 1.0, 1.0, 0.3, true},
        {MemoryPartition::Semantic,     90.0, 3.0, 1.5, 0.3, true},
        {MemoryPartition::Procedural,   90.0, 3.0, 1.5, 0.3, true},
        {MemoryPartition::Preference,  180.0, 4.0, 1.5, 0.3, true},
    };
}

inline PartitionDecayPolicy policyFor(MemoryPartition p) {
    for (const auto& policy : defaultPartitionPolicies()) {
        if (policy.partition == p) return policy;
    }
    return {MemoryPartition::Episodic, 30.0, 1.0, 1.0, 0.3, true};
}

#endif // DESKTOP_PET_PARTITION_POLICY_H
