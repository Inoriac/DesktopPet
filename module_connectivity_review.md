# 模块连通检查

检查日期：2026-09-27。检查对象为当前工作区，包含本轮之前已完成的聊天、截图聊天和记忆接入修改。

修复更新（2026-09-28）：下文是修复前的检查记录，原行号用于定位当时版本。列出的 7 个断点已实施修复，具体变化见文末“修复记录”。MCP 原型、可选依赖缺失与旧空类仍按能力边界描述。

结论：启动、聊天、记忆读取、工具调用、情绪表达、动画和 Launcher 通信已有实际调用链；长期人格成长、主动模式控制、提醒状态回写还有断点。文件被编译、服务被创建和单元测试通过，均不等于业务入口已接通。

初次检查仅新增报告，未修改业务代码。以下为当时待修复的问题。

## 需要修复的连接

### 1. 安静／勿扰模式未接到主动聊天决策

- 入口：聊天工具 `set_proactive_mode`。
- 当前行为：工具把模式写进 `config/agent_proactive_state.json`，并显示“好，我会安静一点”等确认文本。
- 断点：生产代码没有调用 `CompanionProactiveState::mode()`；UI 回调忽略 `quietMinutes`。`canStartProactiveChat()` 和 `proactiveChatTiming()` 只考虑启用状态、忙碌、冷却、性格、情绪等，没有读取主动模式。截图聊天也复用这两个判断，因此同样不受模式约束。
- 用户影响：工具即使返回成功，用户要求安静之后，桌宠仍可能按原节奏主动搭话。`quiet_minutes` 目前在工具描述中也明确是预留值。
- 修复方向：把模式、临时安静截止时间接到统一的主动发言判断，同时覆盖定时聊天和自动截图；为临时安静补齐到期恢复。
- 证据：[工具执行](/E:/Funny-Projects/Desktop-Pet/core/ai/tools/companion_tools.cpp:261)、[UI 回调](/E:/Funny-Projects/Desktop-Pet/ui/petwindow_ai.cpp:154)、[主动判断](/E:/Funny-Projects/Desktop-Pet/core/ai/ai_brain.cpp:336)。

### 2. 性格、关系、自我认知的读取已接入，成长写入未接入

- 入口：`AgentRuntimeServices` 创建 `PersonalityService`、`RelationshipService`、`SelfModelService`，并提供给调用方。
- 已连接：`captureSnapshot()` → `PersonaProjector` → 聊天提示词；性格参数也进入记忆召回和主动频率计算。
- 断点：检索生产代码，`recordEvidence()`、`consolidate()`、`applyEvidence()`、`evolve()` 没有真实业务调用。相关更新目前出现在测试中，聊天事件、触摸、工具结果和反思服务没有驱动这些更新。
- 用户影响：当前情绪会变化；配置的性格会影响回复和频率，但正常使用不会自动形成新的长期性格倾向、关系状态和自我认知版本。`profileGrowth=true` 当前只表示服务可用，不能解释为成长链路已运行。
- 修复方向：从已落库的互动事件产生有来源、可去重的证据，接入关系更新和有节制的性格／自我认知整合；用真实互动至下轮投影的集成测试验证。
- 证据：[服务创建与能力标记](/E:/Funny-Projects/Desktop-Pet/core/ai/runtime/agent_runtime_services.cpp:107)、[成长接口](/E:/Funny-Projects/Desktop-Pet/core/ai/identity/personality_service.h:22)、[关系接口](/E:/Funny-Projects/Desktop-Pet/core/ai/identity/relationship_service.h:15)、[投影读取](/E:/Funny-Projects/Desktop-Pet/core/ai/identity/persona_projector.cpp:108)。

### 3. 提醒存储未接入角色隔离

- 入口：每个 `PetWindow` 创建独立的 `AgentScheduler` 后直接 `load()`。
- 断点：组装入口没有调用 `setStoragePath()`，所有实例默认读写同一个 `config/scheduled_tasks.json`；任务没有在此链路绑定角色 ID。
- 用户影响：先创建提醒再启动另一个桌宠，新实例也会载入该提醒；两边都运行时可能重复提醒。各进程持有自己的任务列表，随后保存整个列表也可能覆盖另一实例的变更。
- 修复方向：按 `profileId` 设置存储位置，并明确同一角色多实例的调度所有权；如需全局提醒，应由单一调度服务管理并显式指定目标角色。
- 证据：[调度器组装](/E:/Funny-Projects/Desktop-Pet/ui/petwindow_ai.cpp:86)、[共享路径](/E:/Funny-Projects/Desktop-Pet/core/ai/scheduler/agent_scheduler.cpp:423)、[Launcher 多进程启动](/E:/Funny-Projects/Desktop-Pet/launcher/main.py:401)。

### 4. 提醒执行后未把完成状态和新时间回写记忆

- 已连接：创建提醒会创建 `TaskShadow`；取消、延后工具会调用 `updateTaskShadowStatus()`。
- 断点：任务到期后，`taskTriggered` 的 UI 消费者只记录情绪事件。一次性任务从调度器移除，记忆仍为 Active；循环任务刷新下次时间，记忆仍保留创建时的时间。延后工具也只写延后动作和分钟数，没有更新记忆中的实际 `next_trigger_at`。
- 用户影响：聊天召回可能拿到已经执行的“待办”或过期的下次提醒时间，与 `schedule_list` 的结果不一致。
- 修复方向：统一发布包含任务状态、执行结果和下次时间的变化事件，再同步调度器、任务记忆和需要展示的历史记录。
- 证据：[创建任务记忆](/E:/Funny-Projects/Desktop-Pet/core/ai/tools/schedule_tools.cpp:46)、[取消／延后回写](/E:/Funny-Projects/Desktop-Pet/core/ai/tools/schedule_tools.cpp:250)、[到期移除](/E:/Funny-Projects/Desktop-Pet/core/ai/scheduler/agent_scheduler.cpp:304)、[结果消费者](/E:/Funny-Projects/Desktop-Pet/ui/petwindow_ai.cpp:217)。

### 5. 提醒工具失败后仍发出成功语义的通知

- 触发条件：调度任务的动画或气泡工具返回失败，例如动画工具未注册、动画状态不可用。
- 当前行为：`executeTask()` 发出 `taskFailed` 后继续更新 `lastTriggeredAt`，最后仍发出 `taskTriggered`。UI 将 `taskTriggered` 无条件解释为 `recordTaskOutcome(id, true)`。
- 用户影响：同一提醒可能同时触发失败和成功情绪；一次性任务即使通知失败也会从调度列表移除。
- 修复方向：区分“到期尝试”“部分成功”“通知成功”“失败”，以实际结果决定情绪回写、完成状态与重试。
- 证据：[执行结果分支](/E:/Funny-Projects/Desktop-Pet/core/ai/scheduler/agent_scheduler.cpp:343)、[无条件成功消费](/E:/Funny-Projects/Desktop-Pet/ui/petwindow_ai.cpp:217)。

### 6. 当前构建的手动整理入口没有连接 Daydream 降级实现

- 当前构建：`DESKTOP_PET_HAS_PRIVATE_REFLECTION=0`，libsodium 和 QtKeychain 能力也为 0。
- 已连接：依赖不足时保留旧版自动 Daydream；它仍可在满足空闲条件时整理记忆。
- 断点：右键菜单始终提供“让我打个盹（整理记忆）”，处理函数只尝试 `sleepCycleCoordinator()`。当前构建没有该对象，所以点击只显示“记忆整理功能未就绪”。
- 修复方向：手动整理接到可用的 Daydream 入口，或根据实际能力隐藏／禁用入口并给出对应说明。
- 证据：[菜单入口](/E:/Funny-Projects/Desktop-Pet/ui/petwindow_interaction.cpp:105)、[入口判断](/E:/Funny-Projects/Desktop-Pet/ui/petwindow_ai.cpp:310)、[自动降级路径](/E:/Funny-Projects/Desktop-Pet/core/ai/ai_brain_loop.cpp:467)。

### 7. 条件性生命周期问题：日记片段服务未随依赖释放

- 触发条件：启用私密反思依赖后，日记片段服务已启动，但睡眠恢复失败；或调用 `AgentRuntimeServices::stop()` 后仍保留该 runtime 对象。
- 断点：失败清理和 `stop()` 会销毁日记服务、私密仓库等对象，却没有先停止、销毁 `m_diaryFragmentService`。片段服务保存这些对象的裸指针，并注册了定时器和延迟 60 秒的孤儿草稿检查。
- 风险：启动失败降级后，延迟检查仍可能通过已释放的仓库指针访问数据。当前构建未启用私密反思，因此本次没有触发此路径；这是从生命周期代码确认的条件性问题。
- 修复方向：先停止并销毁片段服务、取消其待执行工作，再释放它依赖的服务与存储；覆盖启动失败和停止后保留对象的测试。
- 证据：[先启动片段服务](/E:/Funny-Projects/Desktop-Pet/core/ai/runtime/agent_runtime_services.cpp:184)、[失败清理遗漏](/E:/Funny-Projects/Desktop-Pet/core/ai/runtime/agent_runtime_services.cpp:234)、[停止清理](/E:/Funny-Projects/Desktop-Pet/core/ai/runtime/agent_runtime_services.cpp:424)、[延迟访问](/E:/Funny-Projects/Desktop-Pet/core/ai/reflection/diary_fragment_service.cpp:190)。

## 模块连接矩阵

“已接入”表示代码中存在完整的入口、传递和消费路径，不表示已对真实外部服务或全部设备行为完成实机验证。

| 模块 | 实际连接 | 结论与边界 |
| --- | --- | --- |
| Launcher 配置与启动 | AppState → 导出 JSON → `--config`、角色 ID → main → ConfigManager / PetWindow | 已接入；保存后在下次启动使用，没有配置热更新协议 |
| Launcher 聊天 | 本地 socket → send/retry/stop 回调 → AIBrain → ConversationModel → 状态轮询 | 已接入；模型和历史均有返回消费方 |
| 手动聊天、主动聊天、截图聊天 | 统一 AIBrain；截图先视觉识别，再交给对话模型；主动机会共享冷却 | 已接入；主动模式设置仍缺失，见问题 1 |
| 聊天历史与记忆 | 历史恢复、近期对话、临时观察／工具结果、长期记忆 → 准备线程 → 上下文 | 前一轮已接通并测试；提醒记忆另有问题 4 |
| 语义检索 | 准备线程 → ONNX provider → SemanticIndexService → 图召回 | 代码已接入；当前构建有 ORT，模型和 tokenizer 资产存在；未另做真实语义质量评估 |
| 模型分工与调用 | ModelRoleRegistry → ModelRouter → LlmChatService → OpenAI-compatible / Anthropic client | 已接入；角色路由、失败处理与取消有消费方；本轮未调用真实 API |
| 工具与确认 | 本地意图／模型工具调用 → ToolRuntime 策略 → 工具 → 结果消毒 → 对话与记忆 | 已接入；高风险工具的确认信号连接到 UI；MCP 不属于已完成的工具链 |
| 性格、关系、自我认知 | identity 仓库 → PersonaProjector → 对话和行为参数 | 读取已接入，成长输入缺失，见问题 2 |
| 情绪 | 触摸、明确赞许／否定、工具／提醒结果、Daydream 中断 → PetController → EmotionEngine | 已接入；自然语言反馈目前主要靠关键词规则；`llmAppraisalEnabled` 只解析配置，没有模型评估入口 |
| 情绪表达与动画 | expressionRequested → BehaviorManager → Idle 时播放 Happy / Cry / Angry / Fear | 已接入；忙碌时排队，过期丢弃；Surprise 没有动画映射，这是显式处理 |
| 模型渲染与交互 | OpenGL 初始化 → 模型／动画加载 → 注册动画工具；触摸、拖拽、吸附 → 动画状态 | 已接入；本轮未运行可视化桌面验收 |
| 定时提醒 | schedule 工具 → AgentScheduler → 气泡／动画／语音 → 情绪 | 基础链路已接入；角色隔离、记忆回写、失败语义有问题 3–5 |
| 自动记忆整理 | 空闲监测 → Daydream → 记忆巩固；依赖齐全时切换 SleepCycle | 当前可走旧版自动路径；手动入口有问题 6 |
| 内心活动、日记、日记浏览 | 会话完成 → 反思；片段／睡眠 → 日记；OwnerDiaryServer → Launcher | 有组装代码，但当前构建缺少所需依赖，能力未启用；不要按“已能使用”验收 |
| 语音 | 回复来源 → VoiceSynthesisService → Python JSON 协议 → GENIE 合成与播放 → 完成事件 | 代码链路已接入；当前配置关闭，本轮未加载模型或播放音频 |
| 技能 | 技能 CRUD → SkillStore → matcher → 聊天提示词；工具可记录成败 | 已接入；不是独立自动执行器；运行时目录仍为共享的 `runtime/skills` |
| 统计 | 启停／触摸／LlmChatService 调用 → StatisticManager → Launcher 状态快照 | 已接入；当前主要按宠物名称统计 |
| MCP | client / server-process / tool-adapter 原型 | 未接入主程序；adapter 明确返回 `mcp_tool_execution_not_implemented` |
| 旧提醒与空类 | PetReminderManager / PetPersonality；event_handler / sound_engine / platform_utils / trayicon | 旧提醒无生产实例；后几项为空类，不能计为可用模块；实际提醒与语音分别由新模块承担 |

另外，情绪存储按模型名称的哈希定位到 `runtime/emotion`，没有与聊天／记忆一样使用不可变 `profileId`。角色重命名、同名角色以及技能是否应共享，需要统一数据归属规则；当前不能宣称所有数据都已按角色隔离。

## 当前配置与验证

- 读取项目默认配置的非敏感字段：活动配置为 `default`，AI 开启，主动聊天开启，基础机会间隔 180000–300000 ms；自动截图与语音关闭。Launcher 导出的用户配置可能不同。
- 动态性格／情绪节奏适用于主动聊天和自动截图。提醒调度器另有固定 10 分钟执行间隔、23:30–08:00 安静时段；不能把它们理解成同一个定时器或同一套冷却规则。
- 本轮重建并通过 5 组 C++ 测试：AgentRuntimeServices、IdentityState、EmotionRuntime、EmotionProviderContract、Tool。
- 本轮通过 7 组 Python / Launcher 测试：配置、角色 ID、进程管理、模型角色配置、API 连接测试器、聊天客户端／页面、日记客户端。
- 合计本轮 12 组通过。前一轮聊天／记忆的 83 个用例通过记录仍保留；本轮没有重复执行这三组测试。
- 测试说明：身份测试直接调用成长服务，不能证明生产互动已连接成长；客户端模拟测试不能证明当前构建提供日记服务。没有测试覆盖的断点以生产调用链为依据。
- 构建记录：[module-connectivity-build.log](/E:/Funny-Projects/Desktop-Pet/build/module-connectivity-build.log)。测试记录：[核心组件](/E:/Funny-Projects/Desktop-Pet/build/module-connectivity-core-tests.log)、[Launcher](/E:/Funny-Projects/Desktop-Pet/build/module-connectivity-launcher-tests.log)。

建议先修复主动模式控制和长期成长输入，再处理提醒状态闭环及角色隔离；启用私密反思前应修复片段服务的生命周期问题。MCP 和旧占位模块应明确标为尚未实现，避免与可用能力混淆。

## 修复记录（2026-09-28）

1. 主动模式已进入 AIBrain 的共享判断，覆盖定时聊天及自动截图；quiet/focus 暂停主动开口，lively 连续调快节奏，临时安静到期恢复之前模式。状态随角色保存。
2. 新增 `agent_runtime_growth.cpp`，在会话持久化完成及定时维护时消费用户事件。明确的相处反馈更新关系、积累性格证据；实际观察时间和已有独立证据门槛满足后才整合性格，自我认知引用已提交的性格版本。消费检查点和角色锁防止重复学习；普通事实、屏幕观察、模型文字和临时勿扰不用于成长。
3. 提醒保存到 `profiles/<profileId>/scheduled_tasks.json`，同一角色只允许一个实例持有调度锁。只有注册角色唯一时才自动复制旧全局提醒；多角色旧文件保留原样，不猜测归属，需确认归属后迁移。第二个同角色实例仍可聊天，但不能操作该角色已由另一实例持有的提醒。锁随调度器销毁释放。
4. 任务变化和待同步状态一起持久化，创建、延后、安静时间顺延、成功执行、失败重试、取消都会回写任务记忆。一次性成功任务归档，循环任务更新时间；回写失败可在下次维护或重启后恢复。TaskShadow 不再被 Daydream 当作普通事实改写。
5. 气泡通知失败保留任务并在至少 60 秒后重试，不发出成功信号；气泡成功而附带动画失败只记录部分成功，避免重复通知和矛盾情绪。
6. 无 SleepCycle 时，右键整理使用旧版 Daydream 的手动入口，跳过初始空闲等待；新聊天仍可中断整理。AI 关闭、忙碌或整理被禁用时给出对应反馈。
7. 失败清理和正常停止均先停止／销毁日记片段服务，再释放其依赖。定时及延迟任务按生命周期失效；模型请求支持同步返回、停止中断和晚到回调，不再引用已失效的栈变量或仓库。

新增 `ModuleConnectivityTests` 覆盖模式期限与隔离、提醒状态闭环、失败重试、部分成功、重启补写、实例锁、损坏文件保护、真实聊天到关系更新、成长时间窗与重放、手动整理中断、日记依赖释放顺序和请求期间销毁。

本轮验证：主程序 `Desktop_Pet` 重新编译成功；新增连通性测试 13 项通过（含初始化、清理和 11 个业务场景），另有 16 组原有回归测试全部通过，共 17 组测试通过。回归覆盖 Launcher、聊天准备与流式回复、记忆召回、运行时、人格、情绪、工具、睡眠整理和日记客户端。`git diff --check` 通过。构建与测试记录：[构建日志](/E:/Funny-Projects/Desktop-Pet/build/connectivity-fix-tests-build.log)、[新增连通性测试](/E:/Funny-Projects/Desktop-Pet/build/connectivity-fix-integration-tests.txt)、[回归测试](/E:/Funny-Projects/Desktop-Pet/build/connectivity-fix-regression-tests.log)。本轮未调用真实外部模型 API、加载 TTS 模型或进行桌面可视化手动验收。

运行约束：多角色旧全局提醒不会自动分配；外部通知已经送达但进程在保存结果之前崩溃，仍可能在恢复后重复一次（外部副作用无法与本地文件原子提交）。私密日记仍需 libsodium / QtKeychain；本轮修复其连接与生命周期，没有改变加密依赖要求。MCP 与旧占位模块不属于本轮的 7 个断点修复。
