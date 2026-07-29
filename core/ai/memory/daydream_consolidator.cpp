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
    // 固定 Personal：对齐 WorkingMemoryCache::consolidateToStore。源 ShortTerm 经
    // add() 默认 Public，直接拷会把含个人语境的 Episodic 暴露给 includeSensitive=false
    // 的公开检索。由工作记忆巩固生成的长期记忆默认按个人语境处理。
    merged.privacyLevel = PrivacyLevel::Personal;
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
    // 第三支 0.65 当前不可达（shouldUpgrade 保证 mentionCount>=2 或 emotion>=0.7），
    // 保留作防御默认：未来 emotion 系统落地或阈值调整时仍给出合理 confidence。
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
    }
    // drain 期间 upgradeOne/discardOne 改了内存镜像 m_entries，但 rollback 只撤
    // SQLite；commit 路径镜像与盘也可能因 addEntry/removeEntry 顺序存在细微差。
    // drain 自身负责 load() 重读对齐，调用方无需关心镜像一致性。
    m_store.load();
    return stats;
}