#include "daydream_consolidator.h"

#include <algorithm>

#include "memory_store.h"
#include "memory_types.h"

DaydreamConsolidator::DaydreamConsolidator(MemoryStore& store)
    : m_store(store) {}

bool DaydreamConsolidator::shouldUpgrade(const MemoryEntry& entry) const {
    // 对齐 WorkingMemoryCache::shouldConsolidate 的硬编码规则（daydream.md 第七节
    // 决策表 #8：LLM 失败降级兜底用 mentionCount>=2 或 emotionIntensity>=0.7）。
    if (entry.mentionCount >= 2) return true;
    if (entry.emotionIntensity >= 0.7) return true;
    return false;
}

bool DaydreamConsolidator::upgradeOne(const MemoryEntry& source) {
    // 升级为 Episodic 长期记忆（字段对齐 WorkingMemoryCache::consolidateToStore）。
    // 硬编码降级版统一落 Episodic 分区；LLM 版由决策 target_partition 再分流。
    MemoryEntry merged;
    merged.type = MemoryType::Episodic;
    merged.status = MemoryStatus::Active;
    merged.privacyLevel = source.privacyLevel;
    merged.key = source.key;
    merged.summary = source.summary;
    merged.content = source.content;
    merged.tags = source.tags;
    merged.scope = source.scope;
    merged.source = QStringLiteral("consolidation");
    merged.importance = std::min(1.0, source.importance + 0.1);
    merged.strength = merged.importance;
    merged.emotion = source.emotion;
    merged.emotionIntensity = source.emotionIntensity;
    merged.emotionConfidence = source.emotionConfidence;
    merged.mentionCount = source.mentionCount;
    merged.confidence = (source.mentionCount >= 2) ? 0.75
                       : (source.emotionIntensity >= 0.7) ? 0.70
                                                          : 0.65;
    merged.sourceMemoryIds = QStringList{source.id};
    merged.evidence = source.evidence;

    m_store.addEntry(merged); // addEntry 内部 persistEntry 写同一事务
    return m_store.removeEntryById(source.id);
}

bool DaydreamConsolidator::discardOne(const QString& id) {
    return m_store.removeEntryById(id);
}

DaydreamConsolidator::Stats DaydreamConsolidator::runHardcodedDrain() {
    Stats stats;

    // 整 drain 单事务：全成 commit，任一失败 rollback（daydream.md 第五节）。
    if (!m_store.beginTransaction()) {
        return stats; // 开不了事务，放弃；committed=false
    }

    // 读 Hippocampus 分区（inbox）条目，按 createdAt/turn 排序（先入先巩固）。
    QList<MemoryEntry> hippocampusItems;
    for (const MemoryEntry& entry : m_store.all()) {
        if (entry.partition == QLatin1String("hippocampus")) {
            hippocampusItems.append(entry);
        }
    }
    std::sort(hippocampusItems.begin(), hippocampusItems.end(),
              [](const MemoryEntry& a, const MemoryEntry& b) {
                  return a.createdAt < b.createdAt;
              });
    stats.scanned = hippocampusItems.size();

    bool ok = true;
    for (const MemoryEntry& entry : hippocampusItems) {
        if (shouldUpgrade(entry)) {
            if (upgradeOne(entry)) {
                ++stats.upgraded;
            } else {
                ++stats.failed;
                ok = false;
                break;
            }
        } else {
            if (discardOne(entry.id)) {
                ++stats.discarded;
            } else {
                ++stats.failed;
                ok = false;
                break;
            }
        }
    }

    if (ok && m_store.commitTransaction()) {
        stats.committed = true;
    } else {
        m_store.rollbackTransaction();
        // 回滚后内存镜像与盘不一致，调用方应 load() 重读。
    }
    return stats;
}