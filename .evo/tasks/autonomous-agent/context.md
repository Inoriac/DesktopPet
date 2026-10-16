# 自主 Agent 开发上下文

## 需求来源

- 用户确认人物识别和记忆归属优先，再实现通用自主活动，最后接入 QQ 等外部能力。
- 用户要求“做简单 commit，然后进入开发阶段”；设计提交为 `9694046`。
- 主设计：`/Users/huang/Desktop/DesktopPet/contracts/agent/自主Agent-身份记忆与活动运行时-桌面端系分.md`。
- 实施计划：同目录 `自主Agent-身份记忆与活动运行时-桌面端-impl-plan.md`，唯一进度来源。

## 决策记录

- [持续开发授权 2026-10-16] 用户明确要求“你看着往下做吧，每做一个阶段就 commit 一下”。每阶段验证、审查后提交并继续下一阶段，不重复请求阶段确认；不推送远端。
- [命令测试基线修复] QProcess 启动本地编译 fixture 时为 CrashExit，AMFI 日志报告 CMS 签名问题；系统签名 printf/pwd 正常。macOS 测试使用它们验证参数、输出和真实工作目录，其他平台使用独立 helper；增加正常非零退出/异常终止测试。工具新增 exit_status，不改白名单或系统安全设置。全量 CTest 48/48 通过；这不证明本机可执行未具备 CMS 签名的自编译程序。

- [开发授权] 已确认顺序并明确要求进入开发，沿用此授权；每个切片仍执行依赖检查、TDD、审查和 Wave 验证。
- [测试布局] 根目录 `tests/test_*.cpp` + Qt Test；由根 `CMakeLists.txt` 注册，不新增 `tests/CMakeLists.txt`。
- [构建环境] 本机有 Qt 6.8.3：`/Users/huang/Qt/6.8.3/macos`；CMake：`/Users/huang/.qt-build-env/bin/cmake`；已有 `build/` 为本仓库 Debug 配置。测试使用临时库，不操作用户数据库。GUI 与真实 QQ 能力不从单测推定。
- [范围] Task 1 不提前实现多人记忆、会话路由或真实 QQ；可信本地入口兼容与外部身份错误归属防护属于本切片。所有新业务均保持通用 provider/namespace，无 QQ 专用核心分支。
- [code-impl Task 1 设计补全] 按 evoloop-gen-tech-design 补全模式同步 §3.2/§4.1/§5.3：人物 revision CAS、匹配 IdentityBindingAccepted 事件、IdentityBound 同事务 outbox、明确 OriginKind 与历史 source 限定。属于已确认身份对应设计的工程细化，未新增产品范围。
- [code-impl 基线] 修改实现前，现有已构建 CTest 47 组中 45 组通过；FileWebToolTests 的自调用命令退出码 9、PromptRendererTests 的旧默认性格字符串断言失败。此基线来自现有 build 二进制，最终需重新构建再复核，不能直接当作本轮回归。
- [code-impl Task 1 sanity check] §3.2 实现锚点已与源码签名核实一致：`RelationshipService::applyEvidence(const QString&, const RelationshipEvidence&)`、`SqliteIdentityRepository::currentRelationship(const QString&, const QString&) const`、`AgentRuntimeServices::captureSnapshot(const QString&, const QString&) const`、`AgentRuntimeServices::chatPreparationRuntimeMetadata() const`、`SqliteEventRepository::open(const QString&)`、`SqliteRuntimeUnitOfWork::begin(const QString&)`、`EventOutbox::enqueue(RuntimeUnitOfWork&, const EventDraft&)`。
- [code-impl Task 1 持久化边界核对] TaskStore::save 负责构建经历 outbox；仅改 TaskRecord/adapter 会漏 origin。已同步主设计 §3.2 和实施文件清单，包含 task_store.cpp。该调整只保持来源，不改变旧任务生命周期。
- [code-impl Task 1 TDD] 目录测试先因缺 person_directory.h 编译失败；runtime 新场景产生 10 处实际行为失败（空对象默认 owner、外部反馈进入人格证据、未知人物产生关系）；聊天外部/内部空对象失败；任务重启来源丢失失败。对应实现后均变为通过，保留在相应行为测试中。
- [code-impl 验证维护] 默认人格已包含七项 trait，PromptRendererTests 仍断言仅有三项的旧完整句子。按现有产品默认值修正断言，检查七项描述均存在，不依赖排序；不改人格生产逻辑。
- [code-impl Task 1 Wave 回归] 全量编译暴露 ModuleConnectivityTests 的旧 fixture 依赖隐式 owner 和未登记 guest；测试已显式提供 local_owner，并通过 PersonDirectory 取得外部人物 ID，保留原数值和隔离断言。无需放宽生产实现。

- [code-impl Task 2 sanity check] 已验证 §3.3 现有锚点：MemoryExtractor::extractFromUserInput(input, triggerTag)、MemoryPolicy::stageCandidates(candidates, store)、MemoryStore::persistMutationBatch(batch, writtenIds)、loadDatabaseOnly(errorMessage)、MemoryMetadata::mergeContext/semanticRevision、图传播 mayParticipate 回调及 ChatSideEffectQueue writer。新增归属 DTO、共享 ContextSpec 和来源查询接口按主设计 1.2 实施。

- [code-impl Task 2 TDD 与验收] 原始来源校验、错配、关联读写失败、图/临时记忆范围、维护分组、转述对象证明、模型跨人冲突、恢复顺序均取得实际 RED→GREEN。新增损坏 audience 四场景、writer 原置信度门槛和强化时范围/敏感/过期复核；最终完整构建成功，CTest 49/49 通过。聊天缓存断言等待实际异步提交，保留原行为验证。
- [code-impl Task 2 锚点复核] MemoryAttributionService::ingestMemory/recallForContext/migrateAttribution、ChatSideEffectQueue::persistCandidates/migrateNextBatch、MemoryStore::summaryForContext(ContextSpec)、DaydreamConsolidator::recoverMaintenance 已按主设计接入；正式 staging 在迁移写事务内复查，只有精确维护重试租约不阻塞。
- [code-impl Task 3 sanity check] ProfileChatHistoryStore::load/appendFinal、ChatPreparationExecutor::submit/Worker::prepare、AIBrain::submitUserMessage/ActiveDialogueResponse、ContextAssembler::assemble 签名均存在。额外核对 UI 先异步持久化后提交 AIBrain、ProfileChatHistoryStore 按值复制到 QThreadPool、ContextProjection 实际位于 include/ai_types.h，已按主设计 1.3 补齐接入文件与线程边界。
- [code-impl Task 3 原始证据补全] 主设计 §1.3/§3.3/§3.4 v1.3 已同步：MessageObserved 是会话原始消息；UserMessageReceived 保留为处理日志，宿主填写 original_origin_ids/source_message_ids，聚合输入保留多来源。growth 跳过处理日志，反思 alias 回溯已验证原事件，人格/日记继续本地主人范围。历史导入标记 historical_import，自身消息与导入不重放成长或自主回复；外部 request.skills 不读取缺 audience 的学习文本。属于原始事实唯一性和既有功能兼容的修订，沿用持续开发授权。
- [code-impl Task 3 并发与接收边界] 已同步主设计：schema v2 仅门禁，旧版本在写锁内复核迁移；会话读后写事务先预留 writer。消息/回执/导入以事务提交为成功，event_log 后续投递失败由单个后台低频 timer 重投同一 outbox，不漏输入或重发；记忆仍核验真正已提交原事件。外部上下文逐条保留宿主 speaker/message 标签与独立正文，避免参与人列表无法对应发言。显式 external/internal 旧事件同样不得走主人材料 fallback。
- [code-impl Task 3 锚点复核] 已核对 ConversationStore::acceptMessage/prepareContext/publishResponse/recordDeliveryReceipt 与 ConversationService 异步 facade、AIBrain::submitIncomingMessage、ChatPreparationExecutor 权威历史准备、ContextAssembler::assemble 的 scope 门禁、isOriginalOwnerExperience 及反思多来源回溯。ProfileChatHistoryStore 的生产 runtime 路径由 PetWindow 注入，按值复制的存储不持有跨线程 QSqlDatabase。完整构建覆盖新增共享头依赖，统计测试目标亦需 core include，不移除原测试。

## 设计偏差


- [code-impl Task 2 设计补全] 主设计 1.2 明确事件预分配 UUID、writer 查询已提交来源、pending 来源、共享 ContextSpec、按上下文缓存与迁移顺序。现有 GUI 先 stage 后异步落库不能直接证明事件已提交；最终人物去重和冲突须移到/复核于 writer。新写先统一带保守归属后再批量迁移，不长时间暂停写入；这是 §3.3/§5.3 的明确修订，不改变产品范围。
- [code-impl Task 2 转述引用补全] 目录存在性无法证明原文对象。主设计新增宿主来源 DTO 的 resolvedPeople，转述对象须来自已提交事件的宿主解析引用，未验证 ID 返回 UNKNOWN_REFERENCE。现有规则不会凭昵称认人，缺引用的候选保持 unresolved；严格第一人称沿用已验证 speaker。属于来源校验的工程细化，沿用用户自主推进授权。
- [code-impl Task 2 恢复顺序补全] 归属迁移会改变语义 revision，必须先恢复原 maintenance/daydream_maintenance 批次，并在正式 Prepared staging 清空前延后迁移；使用原 changeId/hash/CAS，不重新调用模型。正常 bootstrap 沿用既有睡眠恢复顺序，独立 writer 用单个低频计时器等待。已同步主设计 §3.3，正式睡眠仍归协调器决策。
- [code-impl Task 3 设计补全] 主设计 1.3 明确 SQLite 会话权威切换、宿主连接及自身账号核验、单 worker 异步 facade、通用 AIBrain 会话入口和消息派发幂等表。新增 recordDeliveryReceipt 保存已发生的迟到结果；外部工具派发通过宿主异步接口，第 4 阶段通用执行器接入，不在 GUI 同步阻塞或添加软件专用分支。历史空 runtimePath 只保留旧解析兼容，生产明确启用 v2，失败不回退。沿用用户顺序开发授权，不新增产品方向。

无未同步的设计偏差。initialize 显式启动接口、人物 revision CAS、接受事件校验与 TaskStore 来源持久化均已回填主设计。

## 待回溯设计的发现

无待回溯契约项。inactive 人物管理、合并与拆分尚无本切片入口，后续增加这些操作时须先定义解析/历史更正规则，不能把预留 schema 当作已完成能力。
