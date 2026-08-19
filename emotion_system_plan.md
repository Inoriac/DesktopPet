# 桌宠情绪系统设计方案

> 状态：Phase 1 确定性核心已实现并通过全量测试；Phase 2 表现与上下文接入尚未开始。
> 调研日期：2026-08-16。

## 1. 结论

建议采用“连续心境 + 离散表达 + 事件评价”的轻量混合模型：

- 用 `valence`（愉悦度）和 `arousal`（唤醒度）保存缓慢变化的心境；
- 继续使用现有的 `Neutral/Joy/Sadness/Anger/Fear/Surprise` 作为短时情绪标签；
- 由本地、可测试的评价规则把事件转为情绪冲量，不让 LLM 直接改状态；
- 人格只调整敏感度、恢复速度和表达阈值，关系状态仍然独立；
- 情绪只影响表现、措辞和有限的记忆检索权重，不影响命令执行与核心功能。

这不是完整复刻 OCC、PAD、ALMA 或 EMA。它吸收这些模型中适合桌宠的部分，但把实现复杂度控制在可以调试和验收的范围内。

## 2. 产品目标与边界

### 2.1 目标

1. 桌宠对触摸、任务结果、明确反馈等事件产生连贯而不过度的反应。
2. 短时表情会消退，较长期心境会缓慢回归人格基线。
3. 同一输入在同一状态下得到可复现结果，便于测试和调参。
4. 动画、对话、记忆可以读取同一份情绪快照，但不能互相越权修改状态。
5. 用户可以关闭、重置情绪系统；关闭后不再收集情绪事件或触发情绪表现。

### 2.2 非目标

- 不声称桌宠具有真实感受或意识；
- 不做心理诊断，也不从少量行为推断用户心理状态；
- 不把用户离开、关闭程序、拒绝互动或关闭功能解释为伤害桌宠；
- 不用生气、悲伤、嫉妒、内疚暗示推动用户留存；
- 第一版不实现完整 OCC 分类树、PAD 的 dominance 维度或自主 coping 规划；
- 第一版不让历史记忆自动反复触发负面情绪。

## 3. 调研依据

### 3.1 连续维度适合保存心境

Russell 的环形情感模型表明，情感概念可以按“愉悦/不愉悦”和“唤醒程度”组织在连续空间中。对桌宠而言，这比只保存一个枚举更适合表达“有点开心但很困”和“兴奋开心”的差别。

PAD 在此基础上加入 dominance。PAD 对完整的人格和社会交互建模有价值，但本项目暂时没有足够明确的控制感、地位或自主权行为，因此第一版加入 dominance 会增加参数和歧义，却没有稳定的产品输出。建议保留扩展点，不立即实现。

### 3.2 事件应先被评价，再改变情绪

OCC 的核心价值不是情绪类别数量，而是“事件相对于目标、标准和偏好的意义”决定情绪。EMA 进一步强调评价是持续过程：快速反应和较慢反应可以来自同一评价过程所依赖的信息不断更新，而不必维护两套互相冲突的系统。

因此，本项目不应使用一个随机定时器让 LLM 定期选择心情。应当由已发生的事件、当前状态和有限的评价维度产生状态变化。

### 3.3 情绪、心境、人格应分层

ALMA 明确区分短期 emotion、中期 mood、长期 personality，并让评价结果影响认知和具身表现。这个分层与本项目已有的动画、记忆、人格和关系模型相容。

FAtiMA Toolkit 的工程拆分也值得借鉴：情绪评价、情绪化决策和社会关系判断是独立资产，再由感知-行动循环组合。这支持本方案将状态计算与动画选择分开。

### 3.4 识别、理解、表达必须分开

情感计算研究把识别、理解、生成/表达视为不同问题。桌宠收到一句话后，LLM 可以辅助识别其语义，但识别结果不是最终情绪状态；本地引擎仍需校验置信度、限制幅度并执行冷却策略。

## 4. 当前项目审计

现有代码已经具备一些可复用基础，但还没有真正的宠物情绪状态：

- `core/ai/memory/memory_types.h` 定义了六类 `EmotionType`，记忆已保存情绪、强度和置信度；
- `core/ai/memory/memory_retriever.cpp` 仅对完全相同的情绪标签增加 `emotionIntensity * 1.5`；
- `core/ai/ai_brain_loop.cpp` 使用随机间隔的 `emotion` trigger 请求 LLM 行动，它是表现触发器，不是状态模型；
- `config/animation_state_machine.json` 已有 `Happy/Cry/Angry/Fear`，但没有 `Surprise` 动画；
- `core/ai/context_builder.cpp` 没有向模型提供持久心境；
- `entity/pet_personality.*` 目前只控制提醒概率、时间偏差和语句；
- `core/behavior/behavior_manager.*` 与 `core/controller/pet_controller.*` 仍是空壳；
- 记忆文档已经正确区分用户画像、桌宠人格和关系记忆，情绪不应再建立一个“情绪记忆分区”。

当前最大的设计风险是：随机 `emotion` trigger 可以让表现看起来有变化，却没有因果、连续性和可调试性。第二个风险是把记忆中的情绪标签误当成桌宠当前状态，形成负面记忆自我强化。

## 5. 推荐状态模型

```cpp
enum class EmotionType {
    Neutral,
    Joy,
    Sadness,
    Anger,
    Fear,
    Surprise
};

struct EmotionSnapshot {
    double moodValence = 0.10;       // [-1, 1]，负面到正面
    double moodArousal = 0.35;       // [0, 1]，平静到活跃
    EmotionType active = EmotionType::Neutral;
    double intensity = 0.0;          // [0, 1]
    double confidence = 1.0;         // [0, 1]
    QString sourceEventId;
    QDateTime updatedAt;
    QDateTime expressionExpiresAt;
};
```

状态分为三层：

| 层 | 生命周期 | 数据 | 用途 |
|---|---|---|---|
| 人格基线 | 月/永久 | 基准 valence/arousal、正负敏感度、恢复速度、表达阈值 | 决定“同一件事对不同桌宠影响多大” |
| 心境 mood | 数十分钟到数小时 | 连续 valence/arousal | 对话语气、空闲行为倾向、表达概率 |
| 当前 emotion | 数秒到数分钟 | 离散标签、强度、置信度、过期时间 | 动画、短句、记忆事件标注 |

关系状态不并入这三层。关系可以调节事件的重要性，但不能直接写入情绪状态。例如“熟悉用户的表扬权重略高”是允许的，“关系值低所以桌宠生气”是不允许的。

## 6. 事件与评价

### 6.1 统一事件

```cpp
struct AffectEvent {
    QString id;                    // 用于幂等去重
    QString kind;                  // touch_head/task_succeeded/tool_failed/explicit_feedback...
    QString source;                // user/system/tool/memory
    double goalCongruence = 0.0;   // [-1, 1]，事件对当前目标的利弊
    double novelty = 0.0;          // [0, 1]
    double certainty = 1.0;        // [0, 1]
    double controllability = 0.5;  // [0, 1]
    double relevance = 0.0;        // [0, 1]
    QString agency;                // self/user/system/environment/unknown
    QString outcome;               // ongoing/success/failure/loss/unknown
    double confidence = 1.0;       // [0, 1]
    QDateTime occurredAt;
};
```

第一版只接入可确定的结构化事件：触摸、任务完成、提醒结果、工具成功/失败、用户明确的喜欢/不喜欢反馈。普通对话中含糊的情绪意义默认不产生冲量。

### 6.2 评价到离散情绪

每个候选标签计算一个 `[0, 1]` 分数，最高分超过阈值才成为当前情绪：

| 标签 | 主要信号 | 说明 |
|---|---|---|
| Joy | 正 goal congruence、高 relevance | 成功、明确正反馈 |
| Sadness | 负 goal congruence、已确认 loss、低 controllability | 只用于明确损失，不用于用户离开 |
| Anger | 负 goal congruence、明确外部 agency、较高 controllability | 对用户事件使用更高阈值，默认不主动说话 |
| Fear | 负 goal congruence、低 controllability、不确定且高唤醒 | 工具异常通常不应被放大成 Fear |
| Surprise | 高 novelty、高 arousal | 最短生命周期，可带正负 valence |
| Neutral | 所有分数低于阈值或表达过期 | 稳定回退状态 |

评价结果乘以 `relevance * confidence * personalitySensitivity`，再经过单次冲量上限和同源事件频率限制。规则参数必须来自配置并做严格范围校验。

### 6.3 心境更新与衰减

心境采用半衰期回归人格基线：

```text
decayed = baseline + (previous - baseline) * 2 ^ (-elapsed / halfLife)
next    = clamp(decayed + boundedImpulse, allowedRange)
```

建议默认值：

- valence 半衰期 60 分钟；
- arousal 半衰期 20 分钟；
- 单事件 valence 冲量绝对值不超过 `0.18`；
- 单事件 arousal 冲量绝对值不超过 `0.25`；
- 相同来源每分钟最多计入 3 次；
- 负向冲量默认乘 `0.75`，避免偶发错误长时间污染体验。

衰减按时间差计算，不依赖 tick 次数。这样程序卡顿、休眠或测试使用不同 tick 间隔时，结果仍基本一致。

### 6.4 冷却、滞回和优先级

- 新标签必须比分数阈值高，并比当前标签至少高 `switchMargin` 才切换；
- 同一动画在冷却期内不重复播放，只延长内部情绪有效期；
- 低于 `minExpressionIntensity` 的状态只影响上下文，不播放动画；
- 拖拽、窗口吸附、闹钟、说话等用户可见动作优先于情绪动画；
- 非 Idle 状态收到表现请求时进入有界队列，过期后直接丢弃；
- 负向表现阈值高于正向表现阈值，且不能触发主动打扰用户的气泡。

## 7. 模块边界

建议新增 `core/emotion/`，而不是把计算塞进 AIBrain：

```text
PetController / system signals
            |
            v
      EmotionEngine  ----> EmotionStateRepository
            |
            +---- snapshot ----> ContextBuilder
            +---- annotation --> MemoryStore
            +---- request -----> BehaviorManager ----> AnimationManager
```

### 7.1 EmotionEngine

- 唯一可以修改当前情绪状态的组件；
- 接收 `AffectEvent`，负责去重、衰减、评价、限幅、冷却和快照；
- 使用单调时钟计算运行期时间差，持久化时使用 UTC 时间；
- 发出 `stateChanged(snapshot)` 和 `expressionRequested(label, intensity, expiresAt)`；
- 不调用 LLM、不播放动画、不写长期记忆正文。

### 7.2 BehaviorManager

- 负责动作优先级、互斥、排队、冷却和动画映射；
- `Joy -> Happy`、`Sadness -> Cry`、`Anger -> Angry`、`Fear -> Fear`；
- `Surprise` 在资产补齐前只影响短时状态，不强行映射为其他动画；
- 轻微 Sadness 不播放 `Cry`，避免表现过度；
- 动画无法播放时不回写情绪，也不循环重试。

### 7.3 PetController

作为事件入口和依赖组装层：把触摸、任务、工具和生命周期信号转换为结构化 `AffectEvent`。它不包含评价公式。

### 7.4 AIBrain 与 LLM

现有随机 `emotion` trigger 应逐步停用，替换为本地衰减 tick 和表现请求。LLM 只有两个只读或受控入口：

1. `ContextBuilder` 读取快照，加入简短字段，例如 `mood_valence=0.22`、`mood_arousal=0.41`、`active_emotion=joy`；
2. 可选的“语义事件分类器”返回严格 JSON 的 `AffectEvent` 候选，由 EmotionEngine 校验后决定是否接受。

LLM 不得调用 `set_emotion` 直接写状态，不得自行决定持续时间，不得绕过限幅和冷却。分类失败、超时或置信度不足时按“无情绪事件”处理，而不是猜测。

### 7.5 Memory 与 Daydream

- 记忆中的 emotion 是“这段经历的情绪属性”，不是桌宠当前 mood；
- 沿用现有六类字符串，避免 SQLite 数据迁移；`EmotionType` 后续可移动到共享类型文件；
- 只有达到显著性阈值的事件才把当时快照写入记忆属性；
- Daydream 可以整理这些属性，但不能改变实时情绪状态；
- 召回负面记忆不会自动制造负面情绪。若将来需要，必须产生单独的、有上限的 `memory_recalled` 事件；
- 当前“完全相同标签加 1.5 倍强度”的检索加成偏大。第二阶段应改成较小的 valence/arousal 距离加成，并设置总上限，防止 mood-congruent feedback loop。

## 8. 持久化与隐私

建议由独立 `EmotionStateRepository` 在本地 SQLite 中保存单行状态：

```text
schema_version
mood_valence
mood_arousal
updated_at_utc
personality_revision
```

不持久化当前动画、表达队列或普通事件流水。启动时按离线时长先衰减到人格基线，再以 `Neutral` 表达启动；超过最大离线窗口后直接使用基线。

诊断日志只保存事件类型、数值和拒绝原因，不保存用户原始对话。日志应有容量上限，并可在关闭情绪系统时完全停用。

## 9. 配置草案

```json
{
  "emotion": {
    "enabled": true,
    "baseline": {
      "valence": 0.10,
      "arousal": 0.35
    },
    "decay": {
      "valenceHalfLifeSec": 3600,
      "arousalHalfLifeSec": 1200,
      "maxOfflineDecaySec": 21600
    },
    "impulse": {
      "maxValence": 0.18,
      "maxArousal": 0.25,
      "negativeMultiplier": 0.75,
      "sameSourcePerMinute": 3
    },
    "expression": {
      "positiveThreshold": 0.45,
      "negativeThreshold": 0.65,
      "switchMargin": 0.12,
      "minIntensity": 0.45,
      "durationMs": 12000,
      "cooldownMs": 60000,
      "queueLimit": 3
    },
    "llmAppraisal": {
      "enabled": false,
      "minConfidence": 0.80
    }
  }
}
```

加载器应处理缺失字段、错误类型、NaN/Infinity、负持续时间和倒置阈值。运行期配置使用值拷贝或不可变快照，避免持有热加载后失效的引用。

## 10. 安全与反操纵规则

以下规则应作为测试过的产品约束，而不只是提示词：

1. 用户空闲、退出、拒绝、关闭摄像头/麦克风/联网/情绪功能，不产生负向事件。
2. 用户设置边界、纠正桌宠或删除记忆，不产生 Anger/Sadness/Fear。
3. 情绪状态不降低命令成功率，不阻止关闭、删除、隐私和安全操作。
4. 禁止因情绪生成内疚、威胁、排他、嫉妒、依赖或“只有你能救我”式内容。
5. 负面动画不得自动连续播放，不主动弹出要求安慰的气泡。
6. 不把桌宠状态描述成用户心理状态，不给出医疗或心理诊断。
7. 用户明确反馈优先于模型推断；模型推断不确定时保持中性。
8. 提供关闭、重置到基线和查看当前简化状态的能力。

## 11. 分阶段实施

### Phase 1：确定性核心

- 新增共享情绪类型、`AffectEvent`、`EmotionSnapshot` 和 `EmotionEngine`；
- 实现衰减、评价、限幅、去重、滞回与冷却；
- 增加配置解析和严格边界；
- 增加单元测试，不接 LLM、不改现有动画行为。

完成标准：同一事件序列可复现；所有数值始终在范围内；不同 tick 粒度结果一致；关闭后无状态变化。

### Phase 2：表现与上下文

- 用 `PetController` 接入结构化事件；
- 用 `BehaviorManager` 做动作仲裁并接入现有四种情绪动画；
- 向 `ContextBuilder` 注入只读快照；
- 停用默认随机 `emotion` LLM trigger，保留配置迁移兼容；
- 添加 SQLite 单行持久化和离线衰减。

完成标准：情绪动画不打断高优先级动作；启动不恢复过期表情；LLM 不可直接写状态。

### Phase 3：记忆整合

- 显著事件写入现有 memory emotion 字段；
- 调低并限制情绪检索加成；
- 验证 Daydream 只整理记忆、不修改实时状态；
- 添加负面反馈环路回归测试。

完成标准：负面记忆召回不会独立触发连续负面状态；记忆相关性仍是检索主因。

### Phase 4：可选语义评价

- 仅在明确需要时增加 LLM 事件分类；
- 使用严格 schema、置信度门槛、超时降级、输入去敏和成本限制；
- 用固定语料回放评估误触发率，再决定是否默认开启。

完成标准：关闭 LLM 或网络不可用时，核心情绪系统功能完整；模糊输入不产生高强度负面状态。

## 12. 测试清单

- 边界：所有输入极值、NaN、Infinity、负时间和未来时间；
- 数学：半衰期正确，单步 60 秒与 6 步 10 秒结果近似；
- 映射：成功/损失/外部阻碍/不确定威胁/新奇事件分别映射正确；
- 幂等：重复事件 ID 只生效一次；
- 限流：高频触摸或重复错误不会把状态打满；
- 滞回：阈值附近不会在两种动画间抖动；
- 仲裁：Drag、WindowSit、Alarm、Talk 优先级高于情绪动画；
- 持久化：离线时间正确衰减，损坏记录回退到基线；
- 配置：禁用时不启动 tick、不记录事件、不触发表现；
- 安全：空闲、退出、拒绝、删除记忆和关闭权限均不产生负向状态；
- LLM：非法 JSON、越界值、低置信度和超时全部降级为无事件；
- 记忆：情绪仍是记忆属性，不创建新分区，不形成召回自激循环。

## 13. 明确不采用的方案

| 方案 | 不采用原因 |
|---|---|
| 纯离散状态机 | 无法表达强度、混合感受和缓慢心境，切换容易生硬 |
| 纯 valence/arousal | 容易计算，但无法直接选择现有 Happy/Cry/Angry/Fear 动画 |
| 完整 OCC | 分类和评价变量过多，当前事件与目标模型不足，难以可靠调参 |
| 完整 PAD | dominance 暂无稳定产品语义，增加一维却没有对应行为 |
| LLM 直接维护情绪 | 不可复现、成本高、离线失效，容易越权和产生操纵性内容 |
| 随机定时切换情绪 | 缺乏因果和连续性，用户无法理解，测试也无法稳定复现 |
| 记忆召回直接改 mood | 容易形成负面反馈环并放大检索偏差 |

## 14. 参考资料

1. Russell, J. A. (1980), [A circumplex model of affect](https://doi.org/10.1037/h0077714), *Journal of Personality and Social Psychology*, 39(6), 1161-1178.
2. Mehrabian, A. (1996), [Pleasure-arousal-dominance: A general framework for describing and measuring individual differences in Temperament](https://doi.org/10.1007/BF02686918), *Current Psychology*, 14, 261-292.
3. Ortony, A., Clore, G. L., Collins, A., [The Cognitive Structure of Emotions](https://www.cambridge.org/core/books/cognitive-structure-of-emotions/33FBA9FA0A8D86143DD86D84088F289B), Cambridge University Press.
4. Gebhard, P. (2005), [ALMA: A Layered Model of Affect](https://doi.org/10.1145/1082473.1082478), AAMAS '05, 29-36.
5. Marsella, S. C., Gratch, J. (2009), [EMA: A process model of appraisal dynamics](https://www.stacymarsella.org/publications/abstracts/MarsellaCSR09-abstract.html), *Cognitive Systems Research*, 10(1), 70-90.
6. GAIPS, [FAtiMA Toolkit](https://github.com/GAIPS/FAtiMA-Toolkit), social and emotional intelligence toolkit for virtual characters.
7. Picard, R. W., [Affective Computing](https://mitpress.mit.edu/9780262661157/affective-computing/), MIT Press.
