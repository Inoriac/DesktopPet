#ifndef DESKTOP_PET_DAYDREAM_CONSOLIDATOR_H
#define DESKTOP_PET_DAYDREAM_CONSOLIDATOR_H

#include <QString>

class MemoryStore;

// Daydream 巩固器：把 Hippocampus 分区（待巩固 inbox）里的碎片印象「消化」为
// 结构化长期记忆，并清空 inbox。设计见 daydream.md 第四/六/七节。
//
// 本类是「硬编码降级版」（daydream.md 第七节、决策表 #8 的降级兜底）：
// 不调 LLM，用原硬编码规则（mentionCount>=2 或 emotionIntensity>=0.7）判定
// 升级 vs 丢弃。LLM 版落地后此逻辑作为失败降级兜底保留到 Phase 3。
//
// 整个 drain 在单一 SQLite 事务内执行（daydream.md 第五节）：全部成功才
// commit，任一步失败 rollback，主库回到 drain 前。对象是 MemoryStore 的
// partition=='hippocampus' 子集（ShortTerm/TaskShadow 直接落 SQLite 的 inflow）。
class DaydreamConsolidator {
public:
    struct Stats {
        int scanned = 0;     // 读到的 Hippocampus 条目数
        int upgraded = 0;    // 升级为长期记忆的条目数
        int discarded = 0;   // 判定无价值直接丢弃（清空 inbox）的条目数
        int failed = 0;      // 处理失败的条目数（失败则整批回滚）
        bool committed = false; // true=事务已提交，false=已回滚
    };

    explicit DaydreamConsolidator(MemoryStore& store);

    // 跑一轮硬编码降级巩固，返回统计。事务原子性由内部保证。
    Stats runHardcodedDrain();

private:
    bool shouldUpgrade(const class MemoryEntry& entry) const;
    bool upgradeOne(const class MemoryEntry& source);
    bool discardOne(const QString& id);

    MemoryStore& m_store;
};

#endif // DESKTOP_PET_DAYDREAM_CONSOLIDATOR_H