# 运行概览上下文

## 需求来源

2026-10-20 用户希望检查并改造已有统计、日志系统，在 launcher 有独立界面查看 token 消耗、心情等基本运行属性。用户此前授权自主推进、按阶段 commit，继续有效；不逐阶段等待确认。使用 evoloop-gen-tech-design 和 evoloop-code-impl，保留串行子代理、TDD、审查与阶段验证。

## 决策记录

- [最终集成] 全项目构建成功；非 Launcher/WebCommand 回归 72/72、Launcher 系列 7/7 通过，共 79 个选定测试组。当前注册 80 组，WebCommandTests 因既有 macOS 系统拦截未执行；真实模型/音乐用例仍按原有开关跳过。合成截图覆盖独立页和原生 Launcher 外壳，无页面横向溢出；未改动真实角色数据或调用付费模型。
- [UI 验证方式] 完整 macOS qframelesswindow 外壳在 offscreen 平台存在 native 崩溃，独立页面 offscreen 和完整 Launcher 原生平台分别验证均成功。此处只调整 QA 路径，没有修改依赖或产品行为。
- [code-impl Task 3 sanity check] §3.3.5 的 main 页面生命周期/连接代次、pet_combo 信号先更新 AppState、AsyncLauncherChatClient 操作队列/响应分发及 _app_data_dir 路径已核对一致；profileId 取启动时快照，避免握手期间切换选择导致错绑。
- [launcher CR] 用户操作继续优先，聊天状态与概览两个低优先级合并轮询交替派发，避免慢响应下概览饥饿。离线重新显示时即使保存时间未变也按当前日期重算 today。自主活动和任务状态使用核心实际状态值，不把 failure/running/budget_wait 都显示为等待。
- [集成回归适配] 已有 StreamingDialogueTests 的插件元数据日志回归硬编码全局 log/ai_calls.jsonl。新核心复现读不到预期三条记录后，测试改从该临时角色 runtimeDatabasePath 推导 logs 目录；整组 StreamingDialogueTests 通过。验证仍保留原有元数据/配对检查，不回退生产日志隔离。
- [code-impl Task 2 sanity check] §3.2.5 的 LauncherChatServer 鉴权/限流路径、EmotionEngine::snapshot(const)、AIBrain 状态 getter、sleepCycleCoordinator()->isActive() 和副作用队列日志所有权已验证一致。运行快照不调用 advanceTo，任务只取 status；详细日志仍在原异步队列写入。
- [离线日期核查] 保存文件的 today 属于保存当天；launcher 离线读取时从 daily 取当前本机日期，避免隔天打开误把昨天显示成今天。离线本次运行保持不可用。
- [code-impl Task 1 审查] 按实际客户端调用统计；保留原统计查询和事件接口。已验证首个终态去重、预算拒绝不计、低层关闭重复统计、后台用途归当前 UUID、保存检查点恢复不重复。原有接口用例与新增回归共四个测试目标通过。全项目构建及综合回归在集成后统一执行。
- [提供方用量核查] Anthropic 取消/协议失败原先传空 response，丢失 message_start 已报告输入；Task 1 保留使用量元数据，不返回半截正文。取消/中断只有部分报告时，累计是已知下界，不宣称完整账单。
- [code-impl Task 1 sanity check] StatisticManager/ModelRouter/LlmChatService 签名与 §3.1.5 一致。LlmTests 实际源文件是 test_llm_chat_service.cpp，OpenAI 本地 HTTP fixture 已在 test_anthropic_messages_client.cpp；已纠正文档中不存在的 test_llm.cpp 路径。
- [共享客户端核查] ChatPage 会把所有 operationFailed 显示为聊天故障；运行概览增加独立 overviewFailed(code,message)，以免旧核心不支持概览导致聊天反复弹错误。
- [启动行为核查] launcher 支持多进程，同角色可能被重复启动；profile 统计文件增加非阻塞 QLockFile 单写者锁。取锁失败仅内存记录并提示不能保存，不阻止核心启动，也不覆盖另一个进程统计。
- 工作区 `/Users/huang/Desktop/DesktopPet`，单仓库，开始时干净，基线 `84e6b34`。无 AGENTS.md，evo CLI 不可用，使用本目录记录设计。
- 旧 StatisticManager 可复用，但按名称写全局 log/statistics.json；多角色覆盖和改名归属有问题。旧 AI 详细日志也共用 log/ai_calls.jsonl，缺轮转。角色已有不可变 profileId。
- launcher 使用 PySide6 + Fluent Widgets；复用布局和本地认证管道，新增只读运行查询，不复用整页聊天快照做高频统计。
- 真实模型和私人 QQ 不参与验证。使用临时目录、假模型、本地 socket、合成 UI 数据；Windows 实机步骤交接。已知 WebCommandTests 在 macOS 被 AMFI/XProtect 终止，不在最终本地全套通过声明中包含该项。
- 测试工具：`/Users/huang/.qt-build-env/bin/{cmake,ctest}`；Qt `/Users/huang/Qt/6.8.3/macos`；launcher Python 为 `launcher/.venv/bin/python`。新增测试遵循根目录 tests/test_*.cpp 和 tests/test_*.py。

## 设计偏差

无。

## 待回溯设计的发现

无。
