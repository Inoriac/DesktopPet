# 自主 Agent 阶段代码审查

审查范围：PersonDirectory、runtime 身份 schema、来源标记及其聊天/任务/经历传播链路；以主设计 §1、§3.2、§4.1 与身份 DDL 为准。此记录不代表其他七个切片已实现。

## 实现核对

- 身份键包含 profile、provider、namespace、remoteActorId；昵称仅用于展示，同名不会合并。缺稳定 ID 保留 unresolved。
- owner 保留字面 ID；initialize 以新增行回填旧 subject，不改 relationship_state 的行、时间或版本；失败回滚整批。
- 人物与首个账号在同一事务创建。写事务在查询前取得 SQLite 写锁，避免并发读后升级造成重复人物；异常竞争重读精确键。
- 新账号绑定校验已提交且内容匹配的宿主 IdentityBindingAccepted 事件，使用人物版本 CAS；映射、revision 与 IdentityBound outbox 同事务。相同成功请求幂等；冲突不迁移历史。
- metadata 默认 internal/无人物，本地入口显式 local_owner/owner。显式空 snapshot 不回退 owner；外部 owner 账号与主人指导权限分离。
- 成长消费者仅对存在的已解析人物产生关系效果，未知/内部来源不写关系，并推进 checkpoint，避免堵塞后续事件。
- TaskRecord JSON、TaskStore 经历 outbox、TaskExperienceAdapter 连续保留 origin_kind。只有缺字段的已知旧本地路径进入兼容，非法显式来源保持 internal。
- repository、unit of work 和 outbox 沿用现有连接/销毁约定，未增加跨库事务或软件名称分支。

## 审查修复

- 并发测试在所有线程抵达 barrier 后再处理 open 失败，避免测试挂起。
- 默认人格旧提示词测试只识别三个 traits，已改为检查现有七项描述，不修改生产人格逻辑。
- 全量回归发现 ModuleConnectivityTests 的旧 fixture 省略 origin，并使用未登记的 guest。按新契约补真实人物解析和显式来源，保留原关系数值/隔离断言；不放宽生产代码以信任未知来源。

## 覆盖映射

| 契约 | 实际测试 |
| --- | --- |
| 稳定账号、昵称变化、同名隔离、角色/namespace 隔离 | test_person_directory.cpp 的 resolvePerson 系列 |
| 缺稳定 ID、不自动 owner、存储失败与并发 | 同文件 missingStableId、storeFailure、concurrentFirstResolution |
| 多账号、主人外部账号、关系历史不变 | newAccountWithEvidence、trustedLocalOwner |
| 证据校验、旧版本、未知人物、重复请求 | evidenceMustMatchHostAndScope 数据表、staleRevisionOrMissingEvidence、newAccountWithEvidence |
| 绑定 outbox 失败回滚 | outboxFailure_rollsBackMappingAndRevision |
| 旧库幂等迁移与初始化失败 | migration_existingRelationshipSubjects、initialize_failureRollsBackOwner |
| 聊天本地/外部/内部空对象 | test_chat_preparation_executor.cpp 的 chatPreparation_emptySubject_respectsOrigin |
| 关系与人格权限、未知对象、历史来源、消费幂等 | test_agent_runtime_services.cpp 的 processIdentityEvents 系列 |
| 任务重启仍保留 external | 同文件 taskExperience_preservesExternalOriginAcrossRestart |
| 真实聊天联通、旧关系增长与他人隔离 | test_module_connectivity.cpp 的关系/成长测试 |

## 范围与保留项

- inactive 是预留状态；本切片不提供人物失效、合并、拆分管理入口，不能声称这些操作已经实现。
- 多人记忆范围、会话路由、自主活动与 QQ 接入仍归后续切片。
- 原命令测试退出码 9 已定位为本机 AMFI 拦截自编译子程序，QProcess 报 CrashExit。macOS 成功测试改用系统签名 printf/pwd，保留真实调用、含空格参数、输出与工作目录断言；其他平台用独立 helper。工具仅新增 exit_status 与崩溃报错，新增正常非零退出及异常终止测试。未绕过系统安全设置；完整 CTest 48/48 通过。

最终构建与测试结果见同目录阶段实现总结；进度仅记录在 impl-plan。

## §3.3 人物记忆归属、召回与迁移

本节记录 §3.3 的实现核对、修复与验收证据。

- 归属必须从真实已提交来源取得；队列重试保留 eventId 和时间，非输入事件仍正常落库。
- 相同摘要还须校验命题、人物和 audience；重复原始来源不增加独立证据。转述的 speaker 与 subject 分开，不能把“我的朋友”认作主人。
- 旧无 context 的调用是本地主人兼容，不能跳过范围或逐条借用记录 profile 自授权限。
- 临时工作记忆在进入 query.text 前过滤，图的中间节点与最终强化均复核范围。
- 话题缓存按稳定会话/活动与权限分组，不以每轮变化的 conversationRevision 分组。
- Daydream 与 MemoryOrganizeTool 两条合并路径均保留来源；不兼容的原条目不能被错误标记已替代。
- SQLite v1→v2 升级、关联写失败回滚、JSON 往返、稳定 cursor 分批迁移及迟到来源的语义版本校验。

### §3.3 覆盖映射

| 契约 | 实际测试 |
| --- | --- |
| 两人相同偏好隔离、同人冲突、原始来源幂等与累积 | test_memory_attribution.cpp：ingestTwoSpeakersAndOrigins |
| 原始事件未提交、迟到补全、错配、查询失败 | pendingSourceAndMismatch、sourceLookupFailureDoesNotWrite；test_chat_side_effect_queue.cpp：attributedInputVerifiesCommittedEventAndRepairsPending |
| 转述对象与参与人、已有目录人物仍需原文引用 | reportedSubjectAndParticipantsSurviveConsolidation、describedPeopleNeedCommittedReferences |
| 第一人称活动与“我的朋友”等歧义 | ownActivitiesHaveVerifiedSubject、ambiguousFirstPersonDoesNotClaimOwner |
| 可见共享知识、私聊范围、图中间节点及工作记忆 | audienceAndJsonRoundTrip、privateGraphBridgeAndWorkingMemoryAreInvisible；test_chat_preparation_executor.cpp：prepare_externalContextFiltersWorkingMemoryBeforeQuery |
| 会话话题独立、稳定缓存键、旧任务摘要入口 | contextSwitchKeepsTopics、contextKeyUsesStableConversation、summariesRespectCurrentAudience |
| 相同摘要不同命题、巩固不跨人物、迟到来源使旧提案失效 | sameSummaryDifferentFactsRemainSeparate、consolidationRejectsOtherSubjectAndLateOrigin、modelConflictCannotCrossPeople |
| 维护工具按人物分组合并、来源集合与失败原子回滚 | test_memory_strategy.cpp：testMemoryOrganizeRespectsPeopleAndAtomicOrigins |
| SQLite/JSON 归属往返、schema 升级和未来版本拒绝 | audienceAndJsonRoundTrip、schemaUpgradePreservesBodyAndRejectsFutureVersion |
| 关联写失败、读失败、迁移断点与原正文不变 | associationFailureRollsBack、attributionReadFailureIsNotLegacy、migrationIsAtomicAndIdempotent、migrationFailureKeepsCursorAndBody |
| 旧维护批次在迁移前恢复、正式睡眠 Prepared 阻塞迁移 | test_sleep_cycle.cpp：maintenanceReplaysDurableBatchExactlyOnce、migrationWaitsForSleepRecovery |
| 原聊天异步写入和删除、来源 session 保留、工具及屏幕临时记忆 | test_streaming_dialogue.cpp 的既有相关场景 |

本表只记录实现与测试入口对应关系；最终构建、测试结果和审查结论在完成后另行记录。

### §3.3 审查修复与定向验证

- 将最终归属、去重和冲突放在串行 writer 中，原始输入事件先幂等提交；目录存在性不再被当作原文引用证明。
- 同一 pending origin 补全为 committed 时保留记忆 ID 和独立证据数；直接自述的 reported→fact 只在校验来源后发生。
- 修复无 context 调用逐条使用记录 profile 的隐式授权，默认由 store.profileId 提供本地主人兼容；临时记忆和任务摘要在构造模型材料前过滤。
- 补齐 MemoryOrganizeTool 合并入口的人物分组和原子提交，保留 Daydream CAS 对新增来源的保护。
- audience 存储格式损坏时拒绝读取，不回退为 owner_only；corruptAudienceIsNotLegacy 四个数据场景从实际失败转为通过。
- writer 提交强化前复核当前范围、敏感标记和到期时间；reinforcementRechecksAudienceAtWrite 的三个数据场景通过。candidatesKeepOriginalWritePolicy 保留旧置信度与显式请求门槛。
- 抽取维护恢复为共享函数，在归属迁移前重放原持久化批次；正式或新到达的 Prepared 变更阻塞迁移。没有放松 payload hash、changeId 或语义版本校验；计时器只重试恢复检查，不生成模型决定。
- StreamingDialogue 的缓存断言改为等待实际异步提交，仍验证重试不增加来源/提及次数、待删除内容不进入当前提示词、最终删除进入缓存和数据库；未删除功能断言。

定向验证：MemoryAttributionTests、ChatSideEffectQueueTests、SleepCycleTests、StreamingDialogueTests **4/4 通过**，日志 `/tmp/attribution-cleanup-green-ctest.log`；完整构建与 49 组回归仍待本阶段最终记录。代码审查未发现剩余阻塞项。

### §3.3 最终结论

代码审查通过，Wave 2 五项校验通过。最终完整构建成功；CTest **49/49 组通过**（13.50 秒），日志 `/tmp/memory-attribution-final-clean-build.log`、`/tmp/memory-attribution-final-clean-ctest.log`。`git diff --check` 通过。未改变真实 QQ 接入状态。
