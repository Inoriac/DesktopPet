#ifndef DESKTOP_PET_HYBRID_GRAPH_BUILDER_H
#define DESKTOP_PET_HYBRID_GRAPH_BUILDER_H

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "memory_relation.h"
#include "memory_types.h"

class MemoryRelationGraph;

// 混合建图引擎（设计 §10）：
// - 确定性边（DerivedFrom/Supersedes/CreatedTask）由 MemoryPolicy 写入路径与
//   Daydream 规则直接建立；
// - 本引擎负责 Daydream 巩固批次的共现建图（MentionedWith/Related）、
//   同事件 MentionedWith 建图，以及周期性边维护（衰减/悬空清理/节点边上限）。
// - Embedding 相似候选（HNSW → Related）在 Phase 4.3 完成后接入。
class HybridGraphBuilder {
public:
    struct Policy {
        double relatedWeightPerSharedTag = 0.2;  // 每共享 1 个标签的 Related 权重增量
        double relatedMinWeight = 0.3;           // 共享标签数低于该权重门槛不建边
        double relatedMaxWeight = 1.0;
        int mentionedWithMaxPerNode = 8;         // 单次批次每节点最多新建 MentionedWith 边
    };

    explicit HybridGraphBuilder(MemoryRelationGraph& graph,
                                Policy policy = {0.2, 0.3, 1.0, 8});

    // Daydream 巩固批次建图：对批次产出的长期记忆（Create/Update 结果），
    // 同会话/任务上下文的建立 MentionedWith，共享标签的建立 Related。
    // 全部走 addOrReinforceRelation（重复边合并为 support_count+1）。
    // 返回新建/强化的边数。
    int buildForConsolidationBatch(const QList<MemoryEntry>& results);

    // 写入路径钩子：同一事件（同 source_event_id / 同轮交互）产生的多条记忆
    // 建立 MentionedWith 边。返回新建/强化的边数。
    int buildMentionedWithForEvent(const QList<MemoryEntry>& eventMemories);

    struct MaintenanceStats {
        int danglingRemoved = 0;
        int decayRemoved = 0;
        int capRemoved = 0;
    };

    // 周期性维护：悬空清理 → 弱边衰减 → 节点联想边上限。
    // validMemoryIds：当前存活记忆集合（悬空判定）；
    // capCheckNodeIds：需要检查边上限的节点（通常为本批巩固涉及的节点，控制开销）。
    MaintenanceStats maintain(const QSet<QString>& validMemoryIds,
                              const QStringList& capCheckNodeIds = {},
                              double decayFactor = 0.995,
                              double removeThreshold = 0.15);

private:
    // 提取记忆的上下文聚类提示（session_id > task > tool），同提示视为同情景。
    static QString contextHintFor(const MemoryEntry& entry);

    MemoryRelationGraph& m_graph;
    Policy m_policy;
};

#endif // DESKTOP_PET_HYBRID_GRAPH_BUILDER_H
