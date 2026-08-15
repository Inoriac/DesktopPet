# DesktopPet 记忆框架改进方案

> 基于 hebb-mind 项目对比分析 | 2026-07-23

---

## 一、概述

本文档基于对 **DesktopPet**（C++20 + Qt6 Agent 桌面宠物）和 **hebb-mind**（Python 神经科学启发记忆框架）两个项目的深入分析，提出 DesktopPet 记忆框架的改进方案。

### 1.1 分析结论

DesktopPet 的记忆框架已有良好基础（10 种记忆类型、Ebbinghaus 遗忘曲线、图谱扩展检索、隐私分级），但对比 hebb-mind 后发现三个关键短板：

- **Embedding 向量检索为空壳**：接口已定义但用 Noop 占位，检索纯靠关键词匹配
- **遗忘策略硬编码**：衰减参数 λ 固定，不随访问频率和重要性自适应
- **检索路径单一**：仅有关键词 + 图谱扩展，缺乏语义匹配和多通道融合

好消息是这些问题都有明确且可落地的改进路径。

---

## 二、DesktopPet 记忆框架现状

### 2.1 技术栈

| 维度 | 详情 |
|------|------|
| 语言 | C++20 |
| UI | Qt6 (Widgets + 自绘) |
| 渲染 | OpenGL + TinyGLTF |
| 存储 | SQLite（记忆） + JSON（配置/快照） |
| LLM | OpenAI-compatible API（异步 ChatService） |
| 构建 | CMake |

### 2.2 记忆系统核心模块

```
core/ai/memory/
├── memory_types.h/cpp          # 10 种记忆类型 + 状态 + 情感枚举
├── memory_store.h/cpp           # 主存储（SQLite + JSON 双写）
├── sqlite_memory_repository.*   # SQLite 持久化（6 张表）
├── memory_retriever.h/cpp       # 检索 Pipeline（关键词 + 图谱扩展）
├── memory_policy.h/cpp          # 写入策略（去重、关系发现）
├── memory_extractor.h/cpp       # 从输入提取记忆候选
├── memory_relation_graph.*      # 关系图谱（Supersedes/ConflictsWith/Related）
├── working_memory_cache.*       # 短期上下文缓存（TTL + 条件巩固）
└── memory_json_codec.*          # JSON 序列化（降级快照）
```

### 2.3 当前检索流程

```
用户查询
  ↓
Phase 0: Working Memory 缓存查候选
  ↓
Phase 1: 关键词匹配 + 类型偏好打分
  ↓
Phase 2: Embedding 候选（当前 Noop）
  ↓
Phase 3: 图谱扩展（top3 候选 ×10 邻居，固定策略）
  ↓
Phase 4: 最终重排序
  ↓
Phase 5: 裁剪到 limit
  ↓
Phase 6: 强化检索到的记忆
```

**打分公式（权重硬编码）：**
```
score = keywordScore × 4.0 + typeMatch × 2.0 + strength × 1.6
      + importance × 1.6 + confidence × 1.2 + recency × 0.8 + emotionBoost
```

### 2.4 当前遗忘机制

遗忘曲线采用 Ebbinghaus 指数衰减模型，**衰减参数硬编码**：

> 来源：`MemoryRetriever::decayLambda`（memory_retriever.cpp:322）

| 记忆类型 | λ（衰减速率） |
|---------|-------------|
| Core、Preference | 0.01（几乎不忘） |
| ShortTerm、Working | 0.3（快速遗忘） |
| 其余（Episodic/Semantic/TaskShadow/Relationship/Event） | 0.05（默认） |

注意：衰减是基于「自上次访问天数」的指数衰减 `strength × exp(−λ × daysSinceAccess)`，λ 越大越快遗忘。类型级 λ 分流较粗（仅 3 档），且固定不随访问频率/重要性变化。

```cpp
double computeEffectiveStrength(const MemoryEntry& entry) const {
    const double lambda = decayLambda(entry.type);
    const qint64 daysSinceAccess = ...;
    return entry.strength * exp(-lambda * daysSinceAccess);
}
```

### 2.5 当前巩固机制（现状）与 Daydream 设想

**当前现状（代码层面，无 Daydream）：** 现阶段并不存在「空闲自主整理」这一机制。巩固实际发生在两个同步路径上，都含糊地夹在用户对话流程里：

1. **检索路径内同步清理**：`AIBrain::retrieveMemoryHints`（ai_brain_router.cpp:272）每次检索前同步调用 `m_workingMemoryCache.cleanup(&m_memoryStore)`。清理时对到期项用硬编码条件判定是否巩固：
   - `mentionCount ≥ 2`（重复提及）
   - `emotionIntensity ≥ 0.7`（情感强度高）
   满足则 `consolidateToStore` 写入一条 `Episodic` 长期记忆，否则丢弃。
2. **对话轮次内同步落库**：每轮对话在 ai_brain_router.cpp:182/213 直接 `m_memoryStore.add(...)` 并 `save()`。

**Daydream 设想（本文档引入，尚未实现）：** 作者希望桌宠能在**空闲时刻自主整理记忆**——区别于写日记（录入当天事件），Daydream 是对 Working/Hippocampus 里的碎片印象做分类/去重/冲突合并/重要性再评估（即"消化"）。该机制的完整设计（触发判定、中断回滚、LLM 巩固流程、Hippocampus 改造、决策项）已拆至独立文档 **`daydream.md`**，在基本记忆框架（本文档 P0/P1）落地后再实现。

### 2.6 当前问题清单

| # | 问题 | 影响 |
|---|------|------|
| 1 | Embedding 向量检索未实现（Noop） | 无法处理同义词、语义相关 |
| 2 | 打分公式权重硬编码 | 缺乏实验依据，不同类型未差异化 |
| 3 | 遗忘曲线无自适应 | 不根据访问频率动态调整 |
| 4 | 图谱扩展策略单一 | 不区分关系类型权重 |
| 5 | 巩固条件粗糙 | 大量低价值记忆可能污染长期存储 |
| 6 | `save()` 全量重写 JSON 快照 | 大记忆集下全量 `QJsonDocument::Indented` 重写有 I/O 放大；非实时双写，但语义偏重 |
| 7 | 缺乏可视化/质量评估 | 难以调试和优化 |

---

## 三、hebb-mind 可借鉴设计

### 3.1 项目定位

hebb-mind 是受神经科学启发的 Python AI Agent 记忆框架，命名源自赫布学习规则（"Neurons that fire together, wire together"）。

**核心设计原则：**
- 零外部服务依赖（SQLite + 本地 Embedding + NetworkX）
- 完整的四阶段记忆生命周期：编码 → 巩固 → 检索 → 遗忘
- 动态自适应：记忆留存时间由重要性和访问频率决定，而非固定 TTL

### 3.2 五大可借鉴亮点

#### 亮点 1：三路径并行检索 + RRF 融合

```
用户查询
  ├── Vector Path：余弦相似度 Top-N（overfetch）
  ├── Keyword Path：FTS5/BM25 Top-N（overfetch）
  └── Graph Path：标签匹配 + 1跳邻居 Top-N（overfetch）
         ↓
    RRF（Reciprocal Rank Fusion）融合排名
         ↓
    可选 Cross-Encoder 重排序
```

**RRF 公式：** `fused_score = Σ 1/(k + rank_in_channel)`，每通道贡献 `1/(k+rank+1)`，`k=60`（RRF 论文标准值）。

每通道召回深度 `overfetch = max(top_k×6, rerank_top_n, 30)` 动态计算（searcher.py:107），并非固定 Top-60；60 是 RRF 的 k 常量。融合后再归一化到 [0,1] 供下游复合评分。不依赖各通道分数尺度校准，直接用排名融合——若同一记忆在多个通道排名靠前，融合后分数更高。

**性能数据：**
- Cross-Encoder 重排序（bge-reranker-base）在 LoCoMo 基准提升 +1.61pp
- MemBench 困难类别提升 +30pp 以上

#### 亮点 2：自适应遗忘机制

```
有效半衰期 = 基半衰期 × (1 + k_importance × importance/10 + k_access × accessCount/10)
留存率 = exp(-闲置天数 / 有效半衰期)
遗忘条件：留存率 < threshold（默认 0.3）
```

**分区级差异化策略**（来源：`REGION_FORGET_DEFAULTS`，hebb-mind/scheduler/forgetting_job.py:36-41）：

| 分区 | 基半衰期 | k_importance | k_access | threshold |
|------|---------|-------------|----------|-----------|
| Hippocampus | - | - | - | 不清扫（由巩固清空） |
| Episodic | 30 天 | 1.0 | 1.0 | 0.30 |
| Semantic | 90 天 | 3.0 | 1.5 | 0.30 |
| Preference | 180 天 | 4.0 | 1.5 | 0.30 |
| Procedural | 90 天 | 3.0 | 1.5 | 0.30 |

> 关键事实：hebb-mind 的 **threshold 全区统一为 0.3**，不存在阶梯差异；`k_access` 全区统一 1.5、`Semantic/Procedural` 的 `k_importance` 为 3.0。任意分区可通过 `settings.forgetting_overrides` 覆盖单字段，缺省字段按「分区默认 → 全局默认」回退。

**遗忘示例（按真实参数验算，`eff = base × (1 + ki·(I/10) + ka·(access/10))`，遗忘于 `idle = eff · ln(1/0.3)`）：**
- 高频访问的高价值 Semantic 记忆（importance=8, access=10）→ `eff = 90 × (1 + 3.0×0.8 + 1.5×1.0) = 441` 天 → 约 `441 × 1.204 ≈ 531` 天后遗忘
- 被忽视的低价值 Episodic 记忆（importance=3, access=1）→ `eff = 30 × (1 + 1.0×0.3 + 1.0×0.1) = 42` 天 → 约 `42 × 1.204 ≈ 51` 天后遗忘

#### 亮点 3：LLM 驱动分类巩固

```
Hippocampus 待巩固记忆
  → 召回相关历史记忆（排除 Hippocampus）
  → LLM 决策：分区分类 + 冲突检测 + 标签提取 + 重要性评分
  → 冲突解决：update（合并）/ keep_both（并存）/ discard（丢弃）
  → 写入目标分区，删除源记忆
```

**空输出处理：**
- LLM 判断无价值 → 删除源记忆（如纯闲聊）
- LLM 解析失败 → 保留源记忆，下轮再处理

**会话级批量巩固：** 按 turn 排序 → 分块（5-10 条/块）→ 每块一次 LLM 调用，减少调用次数约 40%

#### 亮点 4：标签共现图谱

同一记忆的标签之间形成完全图共现边，权重为共现次数。相比 DesktopPet 的关系类型图（Supersedes/ConflictsWith/Related），更适合发现「用户同时关心 A 和 B」的隐式关联。

#### 亮点 5：复合评分 + 上下文扩展

**复合评分：** `(recency × wr + importance × wi + relevance × wv) / (wr + wi + wv)`

其中 `relevance = max(keyword_score, vector_cosine)`，权重可配置。

**上下文扩展：**
- Turn 窗口扩展：检索同 session 相邻轮次的记忆
- 图谱标签扩展：从 Top-K 标签出发扩展 1 跳邻居记忆

---

## 四、改进方案

### 4.1 优先级矩阵

| 优先级 | 改进项 | 工作量 | 预期收益 | Token 增量 | 状态/备注 |
|--------|--------|--------|----------|-----------|-----------|
| 🔴 P0 | 启用真实 Embedding 检索 | 2-3 周 | 检索召回率 ↑40%+ | 0 | 索引骨架/下载器已完成，ONNX 推理待做 |
| 🔴 P0 | 实现 RRF 三通道融合 | 1-2 周 | 检索准确率 ↑25%+ | 0 | 待做 |
| 🔴 P0 | 自适应遗忘机制 | 1-2 周 | 重要记忆留存率提升 | 0 | ✅ 完成 + 分区迁移 + 遗忘扫描 |
| 🟡 P1 | Daydream：空闲记忆整理（新建，详见 `daydream.md`） | 2-3 周 | 记忆分类准确率提升 + inbox 主动清空 | 少量 | 待基本框架稳定后 |
| 🟡 P1 | Working Memory → Hippocampus 改造（详见 `daydream.md`） | 1 周 | 巩固决策质量提升 | 0 | 待做 |
| 🟡 P1 | 图谱双图架构 | 1-2 周 | 关系推理能力增强 | 0 | 待做 |
| 🟢 P2 | Cross-Encoder 重排序 | 1-2 周 | Top-10 精度 ↑15-30% | 0 | 待做 |
| 🟢 P2 | 工程优化（去 JSON 冗余、连接池、ID 语义化） | 1 周 | 减少 I/O，提升稳定性 | 0 | 待做 |
| 🟢 P2 | 记忆质量评估与可视化 | 1 周 | 调试效率提升 | 0 | 待做 |

### 4.2 核心改进详述

---

#### A. 检索增强（P0）

##### A1. 启用真实 Embedding

**推荐方案：ONNX Runtime + bge-small-zh-v1.5（int8 量化）**

| 指标 | 数值 |
|------|------|
| 模型大小 | ~90MB |
| 运行时内存 | ~150-200MB |
| 单条推理（M1） | ~30-50ms |
| 单条推理（Intel i7） | ~80-100ms |
| 批量吞吐（batch=16） | 500+ 条/秒 |

**接口设计草图：**
```cpp
class EmbeddingService {
public:
    void initialize(const std::string& model_path);
    std::vector<float> embed(const std::string& text);          // 768 维
    std::vector<std::vector<float>> embedBatch(
        const std::vector<std::string>& texts, size_t batch = 8);
    float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);
};
```

**备选方案：**
- 方案 B（轻量）：Python sentence-transformers 微服务 + gRPC（开发快，但进程间通信开销）
- 方案 C（极致）：Apple CoreML（仅 macOS）或 TensorRT（需 GPU）

**接入点：** 已有 `EmbeddingProvider` 接口和 `memory_embeddings` 表，替换 `NoopEmbeddingIndex` 即可。

##### A2. RRF 三通道融合

```cpp
struct MemoryCandidate {
    int64_t memory_id;
    double score_vector;    // Vector Path 排名贡献
    double score_keyword;   // Keyword Path 排名贡献
    double score_graph;     // Graph Path 排名贡献
    double rrf_score;       // 融合总分
};

std::vector<MemoryCandidate> rrfFusion(
    const std::vector<MemoryResult>& vector_results,   // 各通道 overfetch 召回
    const std::vector<MemoryResult>& keyword_results,  // 各通道 overfetch 召回
    const std::vector<MemoryResult>& graph_results,    // 各通道 overfetch 召回
    int k = 60
) {
    std::unordered_map<int64_t, MemoryCandidate> candidates;
    
    for (int rank = 0; rank < vector_results.size(); ++rank) {
        auto& m = vector_results[rank];
        candidates[m.id].memory_id = m.id;
        candidates[m.id].score_vector = 1.0 / (rank + k);
    }
    // keyword_results 和 graph_results 同理...
    
    for (auto& [id, c] : candidates) {
        c.rrf_score = c.score_vector + c.score_keyword + c.score_graph;
    }
    
    // 按 rrf_score 降序排列，返回 Top-K
    return sortByRRFScore(candidates);
}
```

**权重可配置：**
```cpp
struct RetrievalWeights {
    double vector_weight  = 1.0;   // 语义匹配
    double keyword_weight = 1.0;   // 精确匹配
    double graph_weight   = 0.8;   // 图谱扩展
};
```

##### A3. 检索权重实验调优

- **离线评估：** 用标注数据集测试不同权重组合
- **在线灰度：** 5% 流量用新配置，对比指标
- **自动回滚：** 指标下降 >5% 时自动恢复

---

#### B. 遗忘机制升级（P0）

##### B1. 自适应衰减公式

对齐 hebb-mind 的留存模型（forgetting_job.py），用自然指数而非 log₂：

```cpp
struct AdaptiveDecay {
    double base_half_life;    // 基础半衰期（天）
    double k_importance;      // 重要性调节系数
    double k_access;          // 访问频率调节系数
    double threshold;         // 留存率低于此值触发遗忘
    double min_retention_days = 1.0;  // 硬下限，防止参数病态导致瞬时删除

    // 有效半衰期 = base × (1 + ki·(importance/10) + ka·(access/10))
    // access 不截断，importance∈[0,10]，importance=0 仅不增益而非删除信号
    double effectiveHalfLife(double importance, int access_count) const {
        return base_half_life * (1.0
                                 + k_importance * (importance / 10.0)
                                 + k_access * (access_count / 10.0));
    }

    double retention(double eff_halflife, double idle_days) const {
        if (eff_halflife <= 0.0) return 0.0;
        return std::exp(-std::max(idle_days, 0.0) / eff_halflife);  // 自然指数
    }

    double forgetIdleDays(double eff_halflife) const {
        return std::max(eff_halflife * std::log(1.0 / threshold), min_retention_days);
    }
};
```

> 注意 hebb-mind 用 `exp(−idle/eff)`（e-folding，留存率=threshold 时 idle=eff·ln(1/threshold)）；若为「真半衰期」语义可改 `exp2`，但需同步调整 threshold 的数学含义。移植时建议保持 hebb 原始 `exp` 形式以复用其已调参的默认值。

##### B2. 分区级差异化策略

```cpp
struct PartitionDecayPolicy {
    std::string partition_name;
    double base_half_life;       // 天
    double k_importance;
    double k_access;
    double retention_threshold;  // 低于此值触发遗忘
};

// 默认策略（对齐 hebb-mind REGION_FORGET_DEFAULTS：threshold 全 0.3，k_access 全 1.5）
std::vector<PartitionDecayPolicy> getDefaultPolicies() {
    return {
        {"Hippocampus",  -1,  0.0, 0.0, 1.00},   // 不清扫（由 Daydream 巩固清空）
        {"Episodic",      30,  1.0, 1.0, 0.30},
        {"Semantic",      90,  3.0, 1.5, 0.30},
        {"Procedural",    90,  3.0, 1.5, 0.30},
        {"Preference",   180,  4.0, 1.5, 0.30},
    };
}
```

##### B3. 类型映射（10 种 → 物理分区）

hebb-mind 使用 5 个物理分区。DesktopPet 保留 `Core` 作为逻辑类型，但物理上并入 Semantic，通过 importance 下限和自适应衰减获得近不朽效果；`Procedural` 同时是逻辑类型和物理分区，避免 Daydream 分类结果无法在类型系统中表达。

| 原类型 | 目标分区 | 说明 |
|--------|---------|------|
| Core | Semantic | importance 下限 + 自适应衰减，仍允许事实演化 |
| Preference | Preference | 直接迁移 |
| Semantic | Semantic | 直接迁移 |
| Procedural | Procedural | 技能、操作方法、习得动作 |
| Episodic、Event | Episodic | 合并 |
| Working | Hippocampus | 原 TTL → Daydream 巩固决策 |
| ShortTerm | Hippocampus（待巩固）| Daydream 按 LLM 判定分流 |
| TaskShadow | Hippocampus → 按需转 Semantic | 任务相关沉淀为事实/程序 |
| Relationship | Semantic（+关系标签）| 保留关系链路（关系类型图另建）|

> 改造范围：`MemoryType` 枚举补齐 Procedural；SQLite `memory_items` 持久化物理分区并做一次存量回填。

---

#### C. Daydream：空闲记忆整理（P1，新建）→ 详见 `daydream.md`

桌宠空闲时刻自主整理记忆（"消化"而非"写日记"）。完整设计——系统级全局空闲复合触发、快照 + 内存 staging + generation 取消、最终短事务原子提交、LLM 分类巩固、Working Memory→Hippocampus 改造——已拆至 **`daydream.md`**。

基本框架阶段与 Daydream 的唯一耦合点：**共现图须落 SQLite 表 `tag_cooccurrences`，并与最终记忆变更处于同一个短事务**，见 E 节。

---

#### D. Working Memory → Hippocampus 改造（P1）→ 详见 `daydream.md`

将 TTL 临时缓存改造为工作记忆收件箱（Hippocampus），巩固触发由检索路径同步 cleanup 改为 Daydream 异步 LLM 分类决策，输出从固定 Episodic 改为明确分区。完整对照表见 **`daydream.md`** 第六节。属 Daydream 阶段。

---

#### E. 图谱双图架构（P1）

从单一「关系类型图」扩展为双图：

1. **标签共现图**（新）：同记忆内标签两两建边，权重=共现次数，发现隐式关联（对齐 hebb `KnowledgeGraph.add_tags` 的共现建边逻辑）。**存储须落 SQLite 表 `tag_cooccurrences`**（非纯内存对象），并纳入 Daydream 最终短事务，避免记忆与图状态分离（详见 `daydream.md` 第五节）。
2. **关系类型图**（现有 `MemoryRelationGraph`，已落 `memory_relations` 表）：7 种关系类型——Related / TopicOf / CreatedTask / Supersedes / ConflictsWith / DerivedFrom / MentionedWith（memory_relation.h）。随事务回滚，无残留。

```cpp
// 共现图：内存索引只作查询加速用，真相在 SQLite tag_cooccurrences 表
struct TagCooccurrenceGraph {
    // 查询索引（从表物化，不作为事务真相源）
    std::unordered_map<std::string, std::set<int64_t>> tag_to_memories;
    std::unordered_map<TagPair, int, PairHash> pair_count;

    // 写入必须经 addMemory(db, ...) 走同一 QSqlDatabase 事务
    void addMemory(QSqlDatabase& db, int64_t id, const std::vector<std::string>& tags);
};
```

**图谱扩展策略改进：** 当前 `neighborsOf(id, 10)`（retriever.cpp:150）对 top3 候选各取 10 邻居、固定 `weight×0.5` 加权。改进为区分关系类型权重：
```cpp
double expansionWeight(MemoryRelationType type) {
    switch (type) {
        case MemoryRelationType::Supersedes:    return 0.9;
        case MemoryRelationType::DerivedFrom:   return 0.7;
        case MemoryRelationType::ConflictsWith: return 0.6;
        case MemoryRelationType::Related:       return 0.5;
        case MemoryRelationType::TopicOf:       return 0.4;
        case MemoryRelationType::MentionedWith: return 0.3;
        case MemoryRelationType::CreatedTask:   return 0.2;
    }
    return 0.5;
}
```

---

#### F. 工程优化（P2）

##### F1. JSON 快照写入收窄
> 事实校正：当前**不存在实时双写**——`addEntry` 只写 SQLite（memory_store.cpp:237→persistEntry），JSON 仅在 `save()`（全量重写）和启动期 `importLegacyJson` 迁移时写。`MemoryJsonCodec` 已是按需 `exportSnapshot`/`importLegacyJsonIfNeeded`。
- 收窄 `save()` 调用：对话轮次内 ai_brain_router.cpp:186/213 的 `m_memoryStore.save()` 改为只在关键节点（收尾/退出）全量落盘，日常写库走 SQLite，避免大记忆集下 `QJsonDocument::Indented` 全量重写的 I/O 放大。
- JSON 保留为按需调试快照 / 导出，不作为一致性存储。

##### F2. SQLite 连接池化
```cpp
class ConnectionPool {
    std::queue<std::unique_ptr<QSqlDatabase>> pool_;
    std::mutex mutex_;
    static constexpr int POOL_SIZE = 4;
public:
    Connection borrow();
    void return_(Connection conn);
};
```

##### F3. 记忆 ID 语义化
```cpp
// 当前：纯 UUID
"a1b2c3d4-e5f6-..."

// 改进：类型前缀 + 短 UUID
"pref:java-e5f6", "sem:cpp20-7a8b", "epi:meeting-9c0d"
```

##### F4. 记忆质量评估
追踪指标：
- `retrieval_hit_rate`：被检索后是否被 LLM 实际使用
- `utility_score`：记忆对决策的帮助程度（LLM 反馈）
- `staleness`：未被访问的天数

自动清理：`utility_score < 0.3 && staleness > 90天` → 标记待清理

---

## 实施进度（2026-07-24 核实）

> 已合并 master（含 feature/front 的 psapi 修复与 launcher Python 前端重构，merge commit `166d621`）。记忆测试 39 passed。主程序 `Desktop_Pet` 在 macOS 可编译。

### ✅ 已解决（已实现且编译/测试通过）

| 项 | 交付 | 验证 |
|---|---|---|
| **分区模型** | `partition_policy.h`：5 分区（Hippocampus/Episodic/Semantic/Preference/Procedural），10 类型→分区映射，Core 类型并入 Semantic | 测试 |
| **自适应遗忘** | `PartitionDecayPolicy`（effectiveHalfLife/retention/forgetIdleDays），套 hebb 真实参数（threshold 全 0.3、k_access 1.5、Semantic/Procedural k_importance 3.0） | 算例锁验（441/42 天） |
| **分区持久化 + 存量回填** | `memory_items` 加 partition 列+索引；`ALTER + CASE` 回填；旧 `partition='core'` 迁移到 semantic；insert/load 读写 | 测试 |
| **检索器接入衰减** | `computeEffectiveStrength` 改用 `policy.retention()`，替换硬编码 λ 三档 | 既有衰减测试仍绿 |
| **遗忘扫描器** | `MemoryOrganizeTool` 加 `forget` 模式 + `applyForgettingSweep`（retention<threshold→Expired，不物理删，跳过 Hippocampus/Sensitive/无时间锚点） | 测试 |
| **向量索引骨架（可插拔）** | `sqlite_embedding_index.{h,cpp}`：走 `memory_embeddings` 表，upsert（content_hash 跳过未变）/search（余弦 top-k）/remove；`EmbeddingProvider` 可插拔；`MemoryStore::databaseConnectionName()` 复用同 DB 连接 | Fake provider 测试 |
| **HF 模型下载器** | `model_downloader.{h,cpp}`：hf-mirror 优先 + huggingface 兜底，逐镜像重试/超时/sha256 校验/已存在跳过/全失败降级 | 本地 file:// 镜像测试 |
| **设计决策** | Core 分区去除；遗忘参数套 hebb；性格偏移进 Preference（JSON 基线为吸引子）；Daydream 拆至 daydream.md；Embedding 路径定 ONNX（详见下） | — |

### ⏳ 尚待解决

| 项 | 状态 / 卡点 |
|---|---|
| **ONNX 真实推理后端** | `OnnxEmbeddingProvider` 未写（加载 .onnx + tokenizer + onnxruntime 调用）。当前 embedding 仍用 Noop/Fake，未接真模型 |
| **onnxruntime 库引入** | 项目本体未链接。**已定**：`third_party/onnxruntime`（gitignore + 每平台拉取脚本 + CMake find_library），Windows 安装包带 dll。脚本与 CMake 配置未写 |
| **模型 ONNX 文件** | **已定**模型 = `bge-small-zh-v1.5`。官方仓库仅 PyTorch（无 ONNX）。**已定**：构建期一次性导出 int8 ONNX + tokenizer 打包，运行时不依赖联网。导出脚本未写 |
| **C++ tokenizer** | BGE 用 BERT WordPiece。拟复用 HF `tokenizers` C 绑定或 header-only，不自写算法。未选型/未写 |
| **provider 注入检索链路** | `ai_brain_router.cpp:303` / `agent_core.cpp:73` 两个 `retrieve` 调用点都未传 embeddingIndex，需把真实 index 注入 |
| **RRF 三通道融合** | 计划 A2，待做 |
| **图谱双图架构** | 计划 E，待做 |
| **Daydream 全部** | 拆至 daydream.md，基本框架稳定后 |

### ⚠️ 已识别但未决的关键风险

- HF 直连不稳（本环境 curl 超时 000）→ 已通过 hf-mirror 优先 + 构建期导出打包规避运行时下载。
- onnxruntime 在 Qt+CMake 集成（计划原列风险）→ third_party + find_library 标准解法，Windows/macOS 各一份平台包。
- tokenizer 是 ONNX 路最繁琐处 → 优先复用而非手写。

---

## 五、分阶段实施路线图

### Phase 1（1-2 周）🔴 最小可行 —— P0 项

**目标：** 检索和遗忘的核心能力对齐 hebb-mind，零 token 增量。

| 任务 | 交付物 | 验收标准 |
|------|--------|---------|
| Embedding 检索上线 | `EmbeddingService` 实现 + ONNX 模型集成（替换 `NoopEmbeddingIndex`） | 语义检索可用，单条 <100ms |
| 自适应遗忘参数 | `AdaptiveDecay` + `PartitionDecayPolicy`（对齐他真实参数） | 高重要性记忆留存率显著提升 |
| 分区列 + 存量回填 | `memory_items` 加 partition 列、9→6 分区映射迁移 | 存量记忆不丢失、按分区扫描遗忘 |

### Phase 2（3-4 周）🟡 核心能力 —— P1 项

**目标：** 检索多通道融合 + 图谱双图。

| 任务 | 交付物 | 验收标准 |
|------|--------|---------|
| RRF 三通道融合检索 | `rrfFusion()` + 三路并行 Pipeline | 检索准确率对比基线 ↑20%+ |
| 图谱双图架构 | `TagCooccurrenceGraph`（落 `tag_cooccurrences` 表）+ 7 类关系加权扩展 | 图谱检索召回率提升；共现图随 Daydream 事务回滚 |
| JSON 快照收窄 | `save()` 全量重写改为关键节点触发 | 日常 I/O 走 SQLite，无放大 |

### Phase 2b —— Daydream（详见 `daydream.md`）

基本框架稳定后进入：Working Memory→Hippocampus 改造、检索路径解耦、macOS 空闲补齐、Daydream 新建（空闲触发 + 快照/staging + generation 取消 + 最终短事务 + LLM 巩固）。交付物与验收标准见 **`daydream.md`** 第十一节。

### Phase 3（1-2 月）🔵 进阶优化 —— P2 项

**目标：** 精度打磨 + 工程完善。

| 任务 | 交付物 | 验收标准 |
|------|--------|---------|
| Cross-Encoder 重排序 | ONNX reranker 集成 | Top-10 精度 ↑15%+ |
| 记忆 ID 语义化 | 新 ID 生成策略 + 迁移 | 存量兼容 |
| SQLite 连接池化 | `ConnectionPool` | 并发无死锁 |
| 记忆质量评估 | `QualityTracker` + 自动清理 | 无价值记忆自动归档 |

---

## 六、风险与注意事项

| 风险 | 影响 | 应对 |
|------|------|------|
| ONNX Runtime 在 Qt + CMake 下集成复杂 | 延迟 P0 Embedding 上线 | 备选：gRPC + Python 微服务快速验证 |
| Embedding 模型增加 150-200MB 内存 | 桌面端内存压力 | 可选更小模型（如 bge-micro，~30MB） |
| Schema 迁移兼容性 | 已有数据丢失 | `ALTER TABLE ADD COLUMN`，存量回填 partition 列；`tag_cooccurrences` 新建表 |
| 图谱双图改造并发读写 | 数据竞争 | `std::mutex`（对齐 hebb asyncio.Lock 共享锁）保护图谱内存索引写入 |
| C++ 移植 Python 设计的差异 | 异步/并发模型不同 | 参考 Python 逻辑，用 Qt 并发原语（QThreadPool/QtConcurrent + 信号槽）重写 |
| Daydream 相关（token/空闲误判/macOS 空闲缺口/迟到回调/快照冲突） | 见 `daydream.md` 第八节 | 应对策略详见 `daydream.md` |

---

## 七、待讨论确认

### 已定
- **Core 分区**：**已撤销**，回到 hebb 5 分区。Core 类型并入 Semantic，靠自适应（importance×access 拉伸半衰期）近不朽，addEntry 给 Core 类型设 importance 下限 0.8。旧库 `partition='core'` 迁移到 semantic。
- **遗忘参数**：直接套 hebb-mind 真实默认值（threshold 全 0.3、k_access 全 1.5、Semantic/Procedural k_importance=3.0、Episodic k_importance=1.0），Phase 3 凭 retrieval_hit_rate/utility_score 观测后再按 DesktopPet 自身指标微调。
- **性格/偏好偏移**：落 Preference 分区（JSON 性格预设保留为基线/吸引子不动，潜移默化 delta 作可衰减记忆叠在上面）。
- **Embedding 路径**：定 **ONNX Runtime**（不走 Python 微服务——PyTorch ~2GB 体积对桌宠分发不现实）。模型 = HF 上的 BGE，推理 = C++ onnxruntime 进程内。
- **模型**：`bge-small-zh-v1.5`（中文、~90MB int8、512 维）。
- **模型 onnx 来源**：构建期一次性从官方 `BAAI/bge-small-zh-v1.5`（仅 PyTorch）导出 int8 ONNX + tokenizer，**打包进发行包**，运行时不依赖联网下载（国内 HF 直连不稳）。
- **onnxruntime 引入**：`third_party/onnxruntime`（gitignore + 每平台拉取脚本 + CMake find_library），**因主要面向 Windows**——brew 仅 macOS 开发机方便、不覆盖 Windows 交付，故否。Windows 安装包带 `onnxruntime.dll`。
- **Daydream 决策**（输入口径、中断回滚语义等）→ 详见 `daydream.md` 第九节。

### 待确认（基本框架相关）

| # | 项 | 推荐默认 | 备注 |
|---|---|---|---|
| 1 | tokenizer 选型 | HF `tokenizers` C 绑定 或 header-only BERT tokenizer | 避免手写 WordPiece 算法 |
| 2 | onnxruntime 平台包 | Windows x64 + macOS arm64/x64 release zip | gitignore，脚本拉取 |
| 3 | RRF / 图谱双图排期 | P1 | 待 P0 embedding 通后 |

> Daydream 相关待确认项（空闲阈值、退避、tick、LLM 选型、降级规则保留期等）见 `daydream.md` 第十节。

---

*本文档聚焦基本记忆框架（Embedding 检索 / RRF 融合 / 自适应遗忘 / 分区迁移 / 图谱双图 / 工程优化）。Daydream（空闲记忆整理）已拆至 `daydream.md`，在基本框架落地后实现。已校正 hebb-mind 遗忘参数表、巩固触发归因、JSON 双写夸大、关系类型枚举（7 种）等事实性误差。**实施进度见"实施进度"章节**（P0 自适应遗忘+分区+遗忘扫描+向量索引骨架+模型下载器已完成；ONNX 真实推理待做）。实施前请 review "待确认"项。*
