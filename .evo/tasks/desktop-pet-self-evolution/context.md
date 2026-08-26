# 桌宠自主迭代任务上下文

## 需求来源

- 用户与 Codex 关于 RSI、外置认知机制和 DesktopPet 自主迭代的连续讨论。
- 技术设计：`.evo/tasks/desktop-pet-self-evolution/桌宠自主迭代-桌面端系分.md`。
- 原始设计基线：`master` commit `1502f9f`；当前集成基线包含已交付的 Daydream 与 EmotionEngine，实施时以集成后的 `master` 为准。

## 决策记录

- [2026-08-20] 现阶段只实现角色连续性、反思与只读日记入口；Skill/Tool 自修改治理及基础模型权重/代码自修改留到稳定版本后的实验 fork。
- [2026-08-20] 自主迭代层只依赖 `EmotionStateProvider`：为现有 `EmotionEngine` 提供只读适配器，并保留无副作用的 `NullEmotionStateProvider` 作为降级；不复制情绪算法、衰减、Appraisal 或情绪表。
- [2026-08-20] 人格变化缓慢、稳定、静默；内心活动只保存专门生成的心理摘要，不保存原始 chain-of-thought。
- [2026-08-20] 日记对模型是私人物品；所有者可经模型不可见的只读 IPC 页面访问，Python launcher 不直接读取 SQLite 或密钥。
- [2026-08-20] LLM 初始人格使用独立 `IdentityBaseline`，不复用或改变提醒系统 `PetPersonality`。
- [2026-08-20] 现有 Daydream 继续作为记忆巩固实现；自主迭代只补充睡眠协调、私有心理摘要与日记，不另建并行的 Daydream 算法或存储格式。
- [2026-08-20] 私有正文使用 libsodium XChaCha20-Poly1305 AEAD；每角色密钥由 QtKeychain 保存到 OS Keychain。依赖或 Keychain 不可用时禁用私有反思，不回落明文。
- [2026-08-21] 以现有代码作为迁移起点、以系分作为目标架构：先完成基线对齐与数据迁移约定，再进入功能实现。
- [2026-08-21] 为角色注册表新增不可变 `profileId`；角色名称只用于展示。现有单库记忆在首次升级时按明确规则归属并迁移，禁止静默切库造成旧记忆不可见。
- [2026-08-21] EventLedger 使用同库 durable outbox 补偿“领域提交成功、结果事件追加失败”，不使用未落表的抽象 recovery marker。
- [2026-08-21] 现有 Daydream 先拆分为无副作用 ChangeSet 生成与正式物化，再由 SleepCycle 将 ChangeSet 写入 staging；旧的直接提交入口保留为兼容委托。
- [2026-08-21] 首版 OwnerDiary 只保证启动核心的同一 launcher 生命周期内访问；launcher 重启后重连既有 detached 核心不在本期范围。
- [2026-08-21] 现有 EmotionEngine 没有历史轨迹存储；首版真实 Provider 的 `trajectory()` 返回空列表，不为日记反向扩展情绪 schema。
- [2026-08-21] [code-impl 整体确认] 用户确认按修订后的系分和 impl-plan 开发；§3.2 因文件与测试规模按机械规则拆为 Task 1A/1B，保持串行。
- [2026-08-21] [code-impl 验证约束] 当前 macOS 环境不支持项目构建与测试，用户明确要求不运行；实现仍先写测试再写代码，仅做静态审查，运行验证留待受支持环境。
- [2026-08-21] [code-impl Task 1A 设计回溯] launcher 是 `pets.json` 唯一写者，C++ 只读；§3.2 已补充 `ProfileResolver`、`ProfileDataMigrator`、锁协议、SQLite 完整性/逐表行数校验及降级返回契约。RuntimeUiBridge 生命周期测试移至 Task 1B，私有依赖运行时降级测试移至 Task 4。
- [2026-08-21] [code-impl Task 1A 跨切片边界] Task 1A 只提供 resolver/migrator 与启动身份校验；profile MemoryStore 的迁移执行和 active path 原子消费由 Task 1B `AgentBootstrap` 在构造/启动 `AIBrain` 前完成，禁止先复制后继续写旧库。
- [2026-08-21] [code-impl Task 1A 确认] 用户确认 Task 1A 结果并同意进入 Task 1B；Wave 1 运行验证因 macOS 环境约束继续留待受支持环境。
- [2026-08-21] [code-impl Task 1B 设计回溯] `AIBrain` 可由 PetWindow 提前构造用于 Qt 信号连接，但构造阶段不得打开 MemoryStore；唯一启动入口由 `AgentBootstrap` 在同一调用栈执行 ProfileDataMigrator、消费 active DB/JSON path、一次性初始化 AIBrain storage，再打开 runtime DB。
- [2026-08-21] [code-impl Task 1B 设计回溯] PetWindow 拥有 AIBrain、RuntimeUiBridge 和 AgentRuntimeServices；services 只拥有领域 Repository/Service，bridge/brain 均为 non-owning，并在窗口销毁 UI/渲染对象前先 stop/reset runtime。
- [2026-08-21] [code-impl Task 1B 设计回溯] RuntimeSnapshot 固定 profile、identity baseline schema/hash、可选 personality/relationship/self-model version、config hash 和 capture time；Event private payload 只保存受类型约束的引用；同库领域写入统一使用 RuntimeUnitOfWork 与 outbox 同连接提交。
- [2026-08-24] [code-impl Task 1B sanity check] §3.2.5 实现锚点已静态验证一致：`AgentBootstrap::start` 按 migrator -> `AIBrain::initializeStorage` -> runtime schema/services 组装的顺序执行；Event Schema registry、SQLite repository、RuntimeUnitOfWork/outbox、RuntimeSnapshot/AgentSession 与 PetWindow 所有权及销毁顺序均与系分契约一致。
- [2026-08-24] [code-impl Task 1B 静态验收] impl-plan 中 Task 1B 的计划测试均存在对应定义，`EventLedgerTests`/`AgentRuntimeServicesTests` 及所需源码已注册到 CMake，`git diff --check` 通过；遵循用户约束，RED/GREEN、构建与测试状态均为“未运行，待受支持环境验证”。
- [2026-08-24] [code-impl Task 1B 确认] 用户确认 Task 1B 当前结果并同意进入 Task 2；Wave 2 冲突、测试覆盖、测试位置、CMake 注册与实现锚点均已静态核对，运行验证继续保留为“未运行，待受支持环境验证”。
- [2026-08-24] [code-impl Task 2 系分回溯] 首次 sanity check 确认现有 LlmChatService/ContextBuilder/ConfigManager/StatisticManager 锚点一致，但 §3.3 缺少可测试调用器、路由/请求/投影完整契约且事件名与 Task 1B 冲突，因此在写测试前停止，未产生代码变更。
- [2026-08-24] [code-impl Task 2 方案确认] 用户选择 MVP 适配器方案：ModelCompletionClient + 有序 routes + 类型化 ContextProjection，复用 `ModelCallCompleted`；本期优先基本可用，可增强项留痕后延后。
- [2026-08-25] [code-impl Task 2 sanity check] §3.3.5 实现锚点已静态验证一致：`LlmChatService::requestAsyncWithConfig`、`ContextBuilder::buildSystemPrompt/buildRuntimeContext`、`ConfigManager::getLlmConfig`、`StatisticManager::recordLlmUsage`、`AIBrain::thinkInternal` Dialogue 调用点与 `ModelCallCompleted` Schema 均与系分契约一致。
- [2026-08-25] [code-impl Task 2 静态审查] impl-plan 计划的 14 个用例与 §3.3.7 补充的 2 个 happy path 均有对应定义；ModelRouter/ModelRoleConfig 测试、生产源码和 AgentRuntimeServices 链接依赖已注册到 CMake，tracked 示例配置已同步，本地 `default_common_config.json` 继续按 `.gitignore` 保护；`git diff --check` 与新文件空白检查通过。遵循用户约束，RED/GREEN、构建与测试状态均为“未运行，待受支持环境验证”。
- [2026-08-25] [code-impl Task 2 确认] 用户确认 Task 2 当前结果并同意进入 Task 3；Wave 3 单 Task 无文件冲突，16 个测试定义、测试位置、CMake 注册、Dialogue 调用点与 §3.3.5 实现锚点均已静态核对；运行验证继续保留为“未运行，待受支持环境验证”。

## 设计偏差

- [resolved] 提示词分支曾将 `PromptRenderer::buildVariables` 绑定到 `PetPersonality`；集成后改为 `IdentityBaseline -> PersonaProjector -> bounded slots -> PromptRenderer -> ContextBuilder`，提醒性格不参与 LLM Prompt。
- [resolved] 原设计把 `PetPersonality` 当作 LLM traits/speakingStyle 基线，但实际类型只有提醒概率、时间扰动和提醒话术；已改为新增独立 `IdentityBaseline`。
- [resolved] 原设计只写“成熟 AEAD/OS Keychain”而未指定依赖；已确定 libsodium XChaCha20-Poly1305 + QtKeychain，并补充 nonce、AAD、密钥边界和失败降级。
- [resolved] 仓库不存在设计接口使用的通用 `Result<T, DomainError>`；已在 §2.3 明确由 Task 1 新增共享 `domain_result.h`。
- [resolved] `StatisticManager::recordLlmUsage` 现有签名没有 role/provider/model；已明确保留旧签名并新增维度重载。
- [resolved] SleepCycle 不能直接读取私有 `AIBrain::m_busy`，且 `AgentScheduler` 没有任务冲突查询；已明确新增只读 `isBusy` 和 `hasTaskDueBefore` 接口。
- [resolved] 现有 `IdentityBaseline`、`PersonaProjector` 与 Prompt 测试已在设计提交后合入；实现计划改为扩展现有类型，不再重复创建。
- [resolved] 原设计假定存在稳定 pet profile id，但现有注册表只有可变名称；已补充 UUID `profileId`、启动参数传递和旧数据迁移协议。
- [resolved] 原设计的事件补偿只有 recovery marker 描述、没有表；已改为 `event_outbox` 持久化补偿。
- [resolved] 原设计低估了 Daydream 直接写正式 MemoryStore 与多库 staging 的差异；已增加 ChangeSet 重构前置任务和兼容入口。
- [resolved] 原 OwnerDiary 一次性 token 无法覆盖 launcher 重启后重连 detached 核心；本期明确收缩为同一 launcher 生命周期。
- [code-impl Task 1A sanity check] §3.2.5 现有锚点已验证一致：`PetWindow::setupAiBrain`、MemoryStore 的 SQLite/JSON 路径与导入语义、Python/C++ 角色清单结构、launcher/C++ 启动参数均与系分现状描述一致。
- [resolved] [code-impl Task 1A] §3.2 已定义 `ProfileDataMigrator` 请求/结果、固定迁移锁与完整性校验标准，并新增纯值 `ProfileResolver`；RuntimeUiBridge 与私有能力测试已分别调整到 Task 1B/Task 4。
- [resolved] [code-impl Task 1B sanity check] 原 §3.2 未定义 RuntimeStartRequest/RuntimeSnapshot/EventDraft/EventFilter 的完整类型，且与 AIBrain 构造期 MemoryStore 打开冲突；已回溯为 inert AIBrain + AgentBootstrap 单入口，并补全事件 Schema、consumer authorization、outbox Unit-of-Work 和所有权/销毁顺序。

## 待回溯设计的发现

- 后续实现 Provider 契约时需直接适配现有 `EmotionSnapshot`/`EmotionEngine`，并把 Null Provider 仅作为未注册或读取失败时的 fallback。
- [resolved] Task 1A 已回溯 §3.2：注册表改为 launcher 单写者，补充 ProfileResolver/ProfileDataMigrator 方法级契约与迁移校验；AgentRuntimeServices 生命周期测试归 Task 1B，私有能力运行时降级测试归 Task 4。
- [resolved] Task 1B 已回溯 §3.2：补齐启动/快照/事件/权限/UoW 契约，并把 `main.cpp`、`ui/petwindow.cpp`、`core/configLoader/config_manager.*` 与新增 schema/UoW 类型纳入实现计划。
- [deferred] Task 2 MVP 不实现完整 JSON Schema、provider 结构化错误码、自适应/持久化熔断、精确成本估算和高级多角色配置 UI；当实际 provider 或运维需求出现时再回溯 §3.3。
- [deferred] Task 2 MVP 保留现有 LLM 总量统计，role/provider/model/routeId 先写入 `ModelCallCompleted`；如后续需要分角色费用与调用趋势，再扩展 StatisticManager 的持久化维度。
- [deferred] Task 2 只接入 Dialogue 现有主链；Vision 的统一 Router 接入、Consolidation/Diary 的真实 ContextProjection 数据源分别随现有屏幕识别重构需求和 Task 3/4 领域服务落地后再补。
