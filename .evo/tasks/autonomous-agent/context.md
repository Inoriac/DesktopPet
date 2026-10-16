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

## 设计偏差

无未同步的设计偏差。initialize 显式启动接口、人物 revision CAS、接受事件校验与 TaskStore 来源持久化均已回填主设计。

## 待回溯设计的发现

无待回溯契约项。inactive 人物管理、合并与拆分尚无本切片入口，后续增加这些操作时须先定义解析/历史更正规则，不能把预留 schema 当作已完成能力。
