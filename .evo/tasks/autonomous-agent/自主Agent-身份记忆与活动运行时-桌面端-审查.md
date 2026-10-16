# §3.2 人物识别与外部身份对应：代码审查

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
