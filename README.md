# 🐾 Desktop Pet (3D 桌宠)

一个基于 C++20 / Qt 6 的高性能桌面宠物，并在其上构建一套有安全边界的 Agent 框架。
目标是构建具有持续身份、人物关系和记忆，能够自主选择活动并通过插件获得新能力的 Agent；桌宠是它的表现与交互形式。现有实现包含 3D 表现、本地意图路由、工具运行时、LLM 对话循环、记忆系统、自主活动与通用插件运行时。

> **⚠️ 本项目基于 MateEngine 部分核心逻辑与资源开发，遵循 MateEngine Pro License (v2.1)，严禁商用，仅供学习与研究。**

---

## ✨ 已完成

* **核心引擎**：GLTF 模型热加载、动画状态机、骨骼快照混合过渡，帧率/渲染质量多级调节。
* **桌面交互**：基于身体部位的触摸反馈；Windows 窗口拖动与顶部吸附跟随；3D 约束版的鼠标追踪（整体绕 Y 轴转向 + 头部小范围补偿，保持正立）。
* **生活功能**：会“偶尔忘记”并事后露怯的智能闹钟；互动 / 情绪 / 运行时统计。
* **Agent 框架**
  * **LLM 接入**：OpenAI-compatible，异步请求 + 失败重试 + Token 统计 + 调用日志。
  * **多模型角色路由**：对话、快速提取、记忆整理、日记、视觉可分别配置模型；支持有序 fallback、单次结构修复和进程内短暂熔断。
  * **上下文权限投影**：按模型角色限制可见上下文；人格演化可读取相关完整私密材料；对话、受理和任务只接受按场景选择的片段，任何模型角色都不获得 owner access。
  * **本地意图路由**：时间 / 问候 / LX Music 控制等高频低风险指令绕过 LLM 直接执行。
  * **统一工具体系**：`AITool + ToolRegistry`，函数 Schema 导出与参数校验。
  * **安全运行时**：`ToolRuntime + PolicyEngine`，按 L0–L4 风险等级处置，敏感字段脱敏，文件/命令走白名单 + 安全根目录，禁止解释器绕过。
  * **上下文管理**：`ContextBuilder` 渲染对话上下文，`ContextAssembler` 按模型角色执行预算裁剪与权限投影。
  * **记忆系统**：SQLite 主存储 + 关系图谱 + 标签共现图 + 遗忘曲线 + 情感增强 + 巩固反馈 + Working Memory 缓存；支持 5 分区自适应遗忘、遗忘扫描和可插拔向量索引。
  * **Daydream 记忆整理**：在满足空闲 / 睡眠条件时对待处理记忆做分类、合并、更新或归档；提交前使用 staged changes，中断可回滚，重启可恢复已提交会话。
  * **Agent 主链路**：`AIBrain` 保留低延迟普通聊天，复杂任务进入独立 `TaskOrchestrator`，执行受理、规划、工具、观察与验证；`AgentSession` 固定会话身份和运行时版本快照。
  * **MCP 接入原型**：客户端 / 进程管理 / 工具适配器。
  * **已注册工具**：气泡 / 通知、动画、时间、用户空闲、电池、网络、提醒任务、安全文件读写与目录查看、受控白名单命令（默认关）、网页获取 / 搜索（SSRF 防护）、天气 / 节假日 / 每日简报、LX Music 播控。
  * **技能学习**：可创建、更新、匹配和删除可复用技能，并记录技能执行结果。
* **角色身份与运行时**：每个角色使用稳定 `profileId` 隔离记忆、身份状态和私有数据；支持旧数据迁移、事件账本、outbox、consumer checkpoint、会话快照和能力降级。
* **人格成长**：七维人格与四维关系，支持原始经历去重、事实/感受/推测分层、14 天证据跨度与提交间隔、按维度限幅。睡眠期模型提案经本地校验后，原子提交参数、风格、自我叙事和证据消费状态；用户任务可取消未提交演化。
* **私密反思与日记**：高价值事件可生成加密的简短内心活动；睡眠周期生成每日私人日记。正文使用 XChaCha20-Poly1305 加密，密钥保存在系统 Keychain，缺少私有依赖时不降级写明文。
* **Python 启动器**：原 Qt 控制面板已由 Python launcher 取代，统管角色注册、配置与进程编排；包含只读私人日记页，通过一次性凭据和本地 IPC 分页读取、按篇解密，不直接访问 SQLite 或 Keychain。
* **个性化与界面**：动画表现 / 业务逻辑分离；Dashboard 风格设置界面；Light / Dark 主题整窗平滑过渡；自绘控件（开关、圆形滑块、动画下拉框、胶囊滚动条）；部分配置热加载。
* **可选语音合成**：Python + GENIE / `genie-tts`（GPT-SoVITS 轻量推理），默认关闭，支持中 / 日 / 英预设与自定义角色。
* **QQ 后台插件**：[安装与契约说明](plugins/qq/README.md)。通过 OneBot 读取授权会话、发送文本与核对回执，接入既有人物与自主活动；受控验证已提供，真实 QQ 和 Windows 测试见[验收交接](plugins/qq/验收交接.md)。

---

## 🚧 未完成 / 开发中

* **Embedding 模型分发**：`OnnxEmbeddingProvider`、WordPiece tokenizer、`SqliteEmbeddingIndex` 与生产组装已经实现；模型文件仍需单独准备，缺失或加载失败时语义检索会降级为关键词路径。
* **RRF 三通道融合检索**：向量 / 关键词 / 图谱三路召回 + 排名融合（计划阶段）。
* **图谱检索融合**：关系类型图和标签共现图已落库，尚未作为完整召回通道接入 RRF。
* **成长体验完善**：人格成长已接入模型反思与本地证据门槛；长期真实体验校准、完整情绪历史轨迹、自适应阈值、成长历史 UI 与可视化编辑仍待完善。
* **私人日记增强**：MVP 暂不提供全文搜索、导出、跨设备同步、重连配对或持久化访问审计。
* **聊天体验**：基于情绪的本地简易对话；LLM 自由对话的语气一致性与工具调用反馈打磨中。
* **物理系统 / 节日皮肤 / 多实例**：全局物理模拟、节日限定饰品、多宠物同屏等尚未实现。

---

## 📋 未来计划

* 按[自主 Agent 设计与实施顺序](contracts/agent/自主Agent-身份记忆与活动运行时-桌面端系分.md)继续校准人物识别、记忆归属、会话上下文、插件执行、自主活动与外部实践的长期真实表现。
* 在受支持的 Windows 环境完成启动、对话、工具、记忆恢复、睡眠整理和私人日记的端到端验证。
* 完善 ONNX embedding 模型分发与真实数据校准，随后落地向量 / 关键词 / 图谱三路 RRF 融合。
* 继续完善 `Trigger + Policy + Action` 调度策略，利用现有天气、节假日、电量、网络状态和用户空闲能力提供克制的主动陪伴。
* 记忆质量评估（retrieval_hit_rate / utility_score）与按 DesktopPet 自身指标微调遗忘参数。
* Cross-Encoder 重排序、记忆 ID 语义化、SQLite 连接池等工程优化。
* 在下面列出的量化条件触发后，将聊天编排从分阶段异步流水线迁移为完整 Agent Actor；触发前优先做缺陷修复和定点性能调优。

---

## 💬 聊天异步架构与 Agent Actor 演进

当前聊天链路采用分阶段异步流水线，保留单 active response 的成熟状态机：初始发送入口的 GUI 线程只完成请求校验、pending assistant 发布和不可变任务投递；准备线程读取 persona / memory 并构建 prompt，结果回到 GUI 线程通过 `requestId + generation` 门禁后立即派发网络请求；副作用线程使用独立 SQLite 连接，按 FIFO 提交 runtime event、记忆强化、用户记忆写入和 AI 调用日志，session barrier 提交后才允许反思与资源释放。副作用队列默认容量为 256，满载时优先丢弃 request / response 日志，不以非关键持久化阻塞 GUI 或模型派发。

当前不卡顿保证覆盖初始发送、上下文准备和持久化链路。普通聊天的工具实现仍由现有聊天状态机同步调用；复杂任务的网络/文件查询已有独立执行线程；若单次工具执行开始稳定占用一帧以上，优先将工具执行迁入独立 executor，并把它作为升级完整 Agent Actor 的触发信号。

当前性能预算如下：

* GUI 线程单段聊天逻辑 P95 `< 4ms`。
* 用户消息与 pending assistant 的 UI acknowledge P95 `< 16ms`。
* 本地准备完成到网络派发 P95 `< 50ms`。
* 16ms GUI timer 在 50-100ms 模拟准备延迟期间应持续推进。

满足以下任一条件时，启动完整 Agent Actor 迁移评审：

* 上述任一核心预算在连续两个发布版本的 Windows 性能回归中超限，且定点优化后仍无法恢复。
* 产品需要同时运行 `>= 2` 个相互独立的多轮会话，现有单 active response 状态机不再成立。
* 视觉、语音、工具等形成 `>= 3` 个需要独立取消、优先级和背压策略的并发消息生产者。
* 代表性负载下队列深度连续 5 分钟保持在 `>= 192`（容量的 75%），或非关键日志丢弃率达到 1%。

迁移时沿用现有不可变消息、generation 门禁和 barrier 契约，由单一 Agent Actor 独占会话状态并通过 mailbox 处理优先级、取消与背压；渲染和聊天展示仍只消费状态事件，不直接等待 Actor 内部工作。

---

## 复杂任务与性格演化

调研、方案比较、行程规划和文档总结等请求由本地规则进入复杂任务；也可以输入 `/task 目标` 明确启动。普通闲聊和简单生活指令继续走原有链路。聊天页显示任务进度，并提供继续、暂停、停止、增加预算及结果不明动作的核对入口；补充条件后点击“继续任务”。

默认仅一项未结束的复杂任务，累计限制为 12 次工具调用、2 次重规划和 5 分钟活跃执行时间。等待用户和停机时间不计入；恢复保留预算及原始人格快照。重启后等待用户确认，结果不明的提醒等动作不会自动重放。终结经历通过运行时 outbox 幂等补齐情景记忆和私密感受。

模型分工新增 `agencyDecision`、`taskExecution`、`personalityEvolution`，缺省复用对话服务。活动模型配置的 `taskOrchestration` 可设置预算，参见配置示例。私密日记与内心活动会交给对应角色配置的模型服务商处理；切换连接档案会改变接收方。“私密”约束存储与披露，不表示仅在本地运算。缺少加密或密钥链依赖时，事实经历仍保存，私密感受保留待处理，绝不降级为明文。

---

## 🛠️ 技术栈

* **语言**：C++20（核心）、Python（launcher / 语音）
* **UI / 图形**：Qt 6 Widgets + 自绘控件、OpenGL、TinyGLTF
* **存储**：Qt Sql + SQLite（记忆主存储）
* **构建**：CMake
* **网络 / LLM**：Qt Network、OpenAI-compatible Chat Completion
* **音频 / 语音**：OpenAL / Qt Multimedia、LX Music API、可选 GENIE TTS

---

## 🚀 安装与使用

以下流程以 Windows 10 / 11 为主。当前已实机验证的组合是 Qt 6.5.3 MinGW 64-bit、MinGW 11.2、CMake/Ninja 和 Python 3.12；MSVC 2019+ 也受构建脚本支持，但 Qt 套件必须与编译器 ABI 匹配，不能混用 MinGW Qt 与 MSVC。

### 1. 运行前置

* Git、CMake 3.20+、Ninja 或 Visual Studio 生成器，以及支持 C++20 的编译器。
* Qt 6，包含 Core、Gui、Widgets、Network、OpenGL、OpenGLWidgets、Sql 和 Test 模块。
* Python 3.10+。核心 launcher 与 QQ 插件最低要求较低，但 3.10+ 同时满足当前各可选插件。
* 支持 OpenGL 3.3 Core 的显卡和驱动。
* 可选：libsodium 与 Qt6Keychain 同时存在时启用加密内心活动和私人日记；缺失时普通桌宠仍可构建，但不会降级保存明文私密内容。

角色模型、语音模型和平台凭据不随 Git 提供。默认角色记录会指向 `assets/models/milltina/model/milltina.gltf`；首次启动前须放入自己有权使用的模型，或修改本地 `pets.json` 的 `modelPath`。目录约定见 [模型资产说明](assets/models/README.md)。

### 2. 构建 C++ 核心

克隆仓库后，在 PowerShell 中进入仓库根目录。下面是 MinGW/Ninja 示例；按实际 Qt 安装位置修改三个变量：

```powershell
git clone https://github.com/Inoriac/DesktopPet.git
Set-Location DesktopPet

$qtRoot = 'C:\Qt\6.5.3\mingw_64'
$mingwBin = 'C:\Qt\Tools\mingw1120_64\bin'
$ninjaBin = 'C:\Qt\Tools\Ninja'
$env:PATH = "$mingwBin;$qtRoot\bin;$ninjaBin;$env:PATH"

cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=$qtRoot" `
  "-DCMAKE_C_COMPILER=$mingwBin\gcc.exe" `
  "-DCMAKE_CXX_COMPILER=$mingwBin\g++.exe"
cmake --build build --parallel
```

使用 Visual Studio 2022 和匹配的 Qt/MSVC 套件时，可改用：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH='C:\Qt\6.7.3\msvc2022_64'
cmake --build build --config Release --parallel
```

每次拉取包含 `CMakeLists.txt` 或测试注册变化的代码后，先重新执行 `cmake -S . -B build ...`。Windows 构建会为正式程序运行 `windeployqt`；开发期测试若报 `0xc0000135`，请确认 Qt 与编译器的 `bin` 仍在当前终端 `PATH` 中。

### 3. 安装并启动 launcher

项目正式入口是 Python launcher，不建议直接双击 `Desktop_Pet.exe`。launcher 负责稳定角色 UUID、配置导出、进程日志，以及聊天和私人日记所需的一次性本地 IPC 凭据。

```powershell
$basePython = (Get-Command python.exe -ErrorAction Stop).Source
& $basePython --version
& $basePython -m venv .venv
& .\.venv\Scripts\python.exe -m pip install --upgrade pip
& .\.venv\Scripts\python.exe -m pip install -r launcher\requirements.txt
& .\.venv\Scripts\python.exe launcher\main.py
```

launcher 会查找 `build\Desktop_Pet.exe`、`build\Release\Desktop_Pet.exe` 及仓库内常用 CLion 构建目录。自定义输出目录时，先指定绝对路径：

```powershell
$env:DESKTOP_PET_EXECUTABLE = 'C:\absolute\path\Desktop_Pet.exe'
& .\.venv\Scripts\python.exe launcher\main.py
```

### 4. 首次配置

1. 首次打开 launcher 会创建默认角色 `Milltina` 和稳定的随机 `profileId`。当前界面可以选择已有角色，尚不提供新增角色流程。
2. 在“AI”页开启 AI，在“连接档案”中选择 `openai-compatible` 或 `anthropic-messages`，填写服务 URL、API Key 和服务端实际支持的模型 ID。
3. 点击“测试连接”。这会用当前编辑值发出一次真实的最小 `ping` 模型请求，不只是检查格式。OpenAI-compatible 地址会补 `/chat/completions`，Anthropic 地址会补 `/v1/messages`。
4. 为对话、视觉、快速提取、记忆整理、日记、Daydream、任务受理、任务执行和人格反思选择连接档案与模型。视觉角色应使用实际支持图片输入的模型。
5. 点击“保存配置”，回到“宠物”页选择角色并点击“启动桌宠”。启动操作也会保存当前配置。AI、自主活动、暂停自主活动和自动屏幕聊天是独立开关；默认不会自动开启自主活动。

API Key 会保存在本机 `%APPDATA%\Desktop Pet Team\Desktop Pet\launch_config.json`，并非系统凭据库。请按敏感文件保护，不要提交或粘贴到问题报告中。

### 5. 日常使用

* 在 launcher 的“聊天”页与当前角色对话；Enter 发送，Shift+Enter 换行，Ctrl+F 搜索历史。图片可选择、粘贴或拖入，视觉识别取决于视觉模型配置。
* `/task 目标` 可显式创建复杂任务；任务卡片提供继续、暂停、停止、增加预算和结果不明确认。
* launcher 的“运行概览”显示当前角色状态、模型调用和 token 统计；详细调用元数据位于角色日志。
* 桌宠托盘菜单的“隐藏形象”会隐藏窗口并停止持续渲染，后台聊天和 Agent 运行时可继续；点击托盘图标或选择“显示形象”恢复。选择“退出”才会结束核心进程。
* 配置插件前先退出目标角色，应用配置后重新启动。每个插件的设置、凭据、缓存和日志都应放在仓库外，并按角色隔离。

Windows 用户数据目录：

```text
%APPDATA%\Desktop Pet Team\Desktop Pet\
├── launch_config.json          # launcher 配置，包含本地模型连接凭据
├── pets.json                   # 角色名、模型路径与稳定 profileId
└── profiles\<profileId>\       # 该角色的记忆、会话、统计、日志与插件绑定
    ├── agent_runtime.sqlite
    ├── memory.db
    ├── private_psyche.sqlite
    ├── chat_history.jsonl
    ├── chat\images\
    ├── statistics.json
    ├── logs\ai_calls.jsonl
    └── plugins.json
```

launcher 拉起的核心进程日志另存于仓库的 `log\launcher\`。不要通过删除 SQLite 文件来解决身份、游标或记忆问题；排查前先保留整个角色目录。

### 6. 可选能力

| 能力 | 额外前置 | 接入说明 |
| --- | --- | --- |
| 网页搜索与阅读 | 构建 `desktop_pet_web`；无需搜索 API Key | [Web 插件](plugins/web/README.md) |
| B 站内容阅读 | Python 3.10+；字幕通常需要人工扫码登录；本地 ASR 另备模型 | [Bilibili 插件](plugins/bilibili/README.md) |
| QQ 会话 | NapCat 或 SnowLuma、已登录 QQ、OneBot v11 正向 WS | [下方 QQ 指南](#qq-后台插件可选) |
| LX Music | Windows LX Music、本地开放 API；部分操作需要真实数据库和程序路径 | [LX Music 插件](plugins/lx-music/README.md) |
| 语音合成 | GENIE / `genie-tts` 环境、角色模型与参考音频 | [语音运行时](runtime/voice/README.md) |

插件安装不会替用户打开宿主自主活动总开关。先退出目标角色，预览配置，确认路径和授权后再应用，随后重启角色。模型是否自主使用某个能力还取决于插件授权、全局开关、上下文和预算。

### 7. 基础验证

```powershell
# Ninja 单配置构建
ctest --test-dir build --output-on-failure -j 4

# Visual Studio 多配置构建使用：
# ctest --test-dir build -C Release --output-on-failure -j 4

& .\.venv\Scripts\python.exe -m unittest discover -s tests -p 'test_launcher_*.py'
```

测试出现 `Skipped` 时先确认是否缺少该能力的 Python 包、Qt DLL、真实播放器或平台后端；跳过不能记作该集成已经通过。完整 launcher 说明见 [launcher/README.md](launcher/README.md)，配置结构见 [config/default_common_config.example.json](config/default_common_config.example.json)。

## QQ 后台插件（可选）

QQ 插件通过 OneBot v11 **正向 WebSocket** 连接 NapCat 或 SnowLuma。NapCat/SnowLuma 在已经登录 QQ 的环境中提供 WebSocket 服务端，Desktop Pet 是客户端；插件不安装后端、不负责扫码登录，也不控制 QQ 窗口。

当前支持授权私聊/群聊的文本读取、文本或引用发送、发送回执，以及可选的后台消息观察。图片、语音、视频等媒体只保留占位，不支持理解或发送；断线期间的历史能否补齐取决于后端。

### 1. QQ 前置

* 已登录测试 QQ 的 NapCat 或 SnowLuma，并开启 OneBot v11 **正向 WS 服务端**。
* 后端类型、WS 地址、登录 QQ 号 `selfId`、稳定的 `connectionKey`。
* 明确允许的测试群 `group:<群号>`、测试私聊 `private:<对方QQ号>`，以及用于确认拒绝行为的未授权会话。
* 后端启用鉴权时准备非空的可打印 ASCII token。token 单独放在仓库外的文本文件中，不写入 URL、Git 或验收日志。
* 已通过 launcher 启动过一次目标角色，使 `%APPDATA%\Desktop Pet Team\Desktop Pet\profiles\<profileId>` 存在。

NapCat 与 SnowLuma 的历史接口和发送证据不同，需要分别验收，不能用其中一个的结果代替另一个。

### 2. 创建 QQ 独立环境和设置

在仓库根目录执行。先从角色清单取得真实 UUID，不要使用角色显示名或自行编造 UUID：

```powershell
$petDataDir = Join-Path $env:APPDATA 'Desktop Pet Team\Desktop Pet'
$pets = Get-Content -LiteralPath (Join-Path $petDataDir 'pets.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$pets | Select-Object name, profileId

$profileId = '替换为上面目标角色的小写UUID'
$profileDir = Join-Path $petDataDir "profiles\$profileId"
$qqState = Join-Path $env:LOCALAPPDATA "DesktopPet\qq\$profileId"

& .\.venv\Scripts\python.exe -B plugins\qq\setup.py --state-dir $qqState
if ($LASTEXITCODE -ne 0) { throw 'QQ 插件环境创建失败，请查看上方输出' }

$qqPython = Join-Path $qqState 'venv\Scripts\python.exe'
$qqSettings = Join-Path $qqState 'settings.json'
$qqToken = Join-Path $qqState 'token.txt'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

$settings = [ordered]@{
  backend = 'napcat' # 或 snowluma
  wsUrl = 'ws://127.0.0.1:3001'
  selfId = '999999999'
  connectionKey = 'pet-qq-account'
  stateDir = $qqState
  tokenFile = $qqToken
  allowedConversations = @('group:123456789', 'private:234567890')
  requestTimeoutSeconds = 10
}
[System.IO.File]::WriteAllText(
  $qqSettings, ($settings | ConvertTo-Json -Depth 5), $utf8NoBom)
```

用编辑器把后端 token 写入 `$qqToken`，保存为 UTF-8 无 BOM。后端明确未启用 token 时，执行 `$settings.Remove('tokenFile')` 后重新写入 `settings.json`，不要留下指向空 token 文件的配置。

每个角色和 QQ 账号使用独立 `stateDir`。更换登录账号时同时更换 `connectionKey` 和 `stateDir`，不要复用旧消息日志。`selfId` 必须等于后端实际登录账号；插件连接后会先核对，不匹配时拒绝继续读取或发送。

### 3. 预览并接入角色

先退出目标桌宠。首次建议关闭 source 和自主授权，只验证显式只读调用：

```powershell
& $qqPython -B plugins\qq\configure.py --profile-dir $profileDir --settings $qqSettings --source off --autonomous off
& $qqPython -B plugins\qq\configure.py --profile-dir $profileDir --settings $qqSettings --source off --autonomous off --apply
```

第一条命令只输出预览；第二条才更新该角色的 `plugins.json`，并在 `qq-backups` 下保存备份。重复应用相同配置不会重复写入。配置变化后重启该角色。

权限边界如下：

* 本地主人始终只能使用 Skill、`status/list/read/receipt`，没有 `send`。
* `--autonomous on` 才给内部自主活动只读权限，并给每个精确外部 QQ 会话启用 `read/send/receipt` 自主调用；内部活动不能任意选择联系人主动开聊。
* 外部会话没有 `list`，也不能读取或发送到另一个会话。
* `--source on|off` 只控制后台接收；`--autonomous on|off` 只控制本插件授权；launcher 的“自主活动”和“暂停自主活动”仍是独立总开关。

### 4. 先做只读连通验证

PowerShell 5 需显式使用 UTF-8。用对象生成 JSON，避免手写转义：

```powershell
$OutputEncoding = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)

@{} | ConvertTo-Json -Compress |
  & $qqPython -B plugins\qq\adapter.py --settings $qqSettings --operation status
@{} | ConvertTo-Json -Compress |
  & $qqPython -B plugins\qq\adapter.py --settings $qqSettings --operation list
@{ chat_id = 'group:123456789'; limit = 10 } | ConvertTo-Json -Compress |
  & $qqPython -B plugins\qq\adapter.py --settings $qqSettings --operation read
```

先确认账号、白名单、群聊/私聊分流、中文、emoji、引用和分页。随后可重新配置为 `--source on --autonomous off --apply`，从允许会话发送测试消息，确认只归档、不自主回复。最后确有需要时再配置 `--source on --autonomous on --apply`，并在 launcher 中开启且取消暂停自主活动。

宿主验收看实际 `discover/load/invoke` 调用、观察归档、QQ 客户端收到的消息和发送回执，不以模型口头说“已经发送”作为成功依据。主体可以选择等待，并非每条消息都必须回复。

### 5. 发送与回执验收

适配器直调会绕过宿主的上下文授权，只能用于自己有权操作的测试群或测试私聊。每项先发一条，查看实际 QQ 客户端，再用返回的 `receipt_id` 查询：

```powershell
@{
  chat_id = 'group:123456789'
  text = 'QQ插件验收文本'
  reply_to = ''
} | ConvertTo-Json -Compress |
  & $qqPython -B plugins\qq\adapter.py --settings $qqSettings --operation send

@{
  chat_id = 'group:123456789'
  receipt_id = '替换为上一步返回的receipt_id'
} | ConvertTo-Json -Compress |
  & $qqPython -B plugins\qq\adapter.py --settings $qqSettings --operation receipt
```

`send` 返回 `unknown` 时，消息可能已经到达。此时只能查看客户端并使用原 `receipt_id` 查询，绝不能再次发送同一消息。完成真实验收时还要覆盖私聊、群聊、引用、字面 `[CQ:at,qq=all]`、错误 token、账号不匹配、断线恢复和未授权会话。

### 6. 无真实 QQ 的受控回归

```powershell
& $qqPython -B tests\test_qq_adapter.py
& $qqPython -B tests\test_qq_delivery.py
& $qqPython -B tests\test_qq_configure.py

cmake -S . -B build "-DQQ_TEST_PYTHON=$qqPython"
cmake --build build --target qq_plugin_tests --parallel
ctest --test-dir build -R '^QQ' --output-on-failure
```

Visual Studio 多配置构建给后两条命令加 `--config Release` 和 `-C Release`。运行 `QQPluginTests` 前应让 Qt 与编译器的 `bin` 位于 `PATH`；否则 Windows 可能报 `0xc0000135`。缺少 `websockets` 时部分 QQ 测试会标为跳过，不能算通过。

受控回归只证明插件协议、单次派发、回执、权限和宿主链路，不代表真实 QQ 已通过。完整配置、运行边界与回滚见 [QQ 插件文档](plugins/qq/README.md)，NapCat/SnowLuma 的 A1-A11、H1-H11 实机表见 [QQ 验收交接](plugins/qq/验收交接.md)。

---

## 📂 项目结构

```text
Desktop-Pet/
├── core/          # 核心逻辑（动画、AI/Agent、记忆、工具、调度）
│   └── ai/        # LLM / 路由 / 安全运行时 / 记忆 / MCP / Agent
├── engine/        # 渲染引擎（OpenGL、模型加载、音频）
├── entity/        # 宠物实体与个性化数据
├── ui/            # 设置界面、自绘控件、托盘
├── launcher/      # Python 启动器（角色注册、配置、进程编排）
├── assets/        # 模型、动画、配置、图标
├── config/        # 默认配置与状态机定义
├── contracts/     # Agent 演进设计、插件开发指导与能力准入契约
├── plugins/       # Web、B 站、QQ、LX Music 等可选能力包
├── runtime/       # 可选语音运行时说明；本地生成内容不入库
├── tests/         # Qt Test 单元测试
└── third_party/   # TinyGLTF、ONNX Runtime 等第三方依赖
```

---

## 📄 协议与版权

本项目遵循 **MateEngine Pro License (v2.1)**，详见 [LICENSE](LICENSE.md)。

* **Developer**：Inoriac
* **Original Engine**：[Mate-Engine](https://github.com/shinyflvre/Mate-Engine) by Johnson Jason
* **Assets**：Copyright © 2025 **Shiny**，按非商业许可使用。

**严禁将本项目用于任何商业用途（含但不限于 Steam / itch.io 销售、付费订阅）。Strictly non-commercial.**

---

## 📖 相关文档

* [自主 Agent：身份、记忆与活动运行时设计](contracts/agent/自主Agent-身份记忆与活动运行时-桌面端系分.md)：后续实施主入口，含空闲定义、数据迁移、切片顺序与验收门槛
* [插件系统开发指导](contracts/tools/插件系统开发指导.md)：Plugin、Skill、Tool 的定位、按需加载与验收要求
* [插件运行时接入](contracts/tools/插件运行时接入.md)：当前配置、权限和调用方式
* [Web 插件](plugins/web/README.md)：匿名网页搜索、正文分页、缓存与 SSRF 边界
* [Bilibili 插件](plugins/bilibili/README.md)：视频材料、字幕、登录与可选本地 ASR
* [QQ 插件](plugins/qq/README.md)：OneBot 前置、角色接入、权限、回执与回滚
* [QQ Windows 验收交接](plugins/qq/验收交接.md)：NapCat / SnowLuma 的真实平台验收表
* [LX Music 插件](plugins/lx-music/README.md)：可运行的音乐工具与 Skill 接入、Windows 实测步骤
* [可选语音运行时](runtime/voice/README.md)：GENIE 数据、角色模型与环境准备
* [启动器说明](launcher/README.md)：配置、运行与主题共享
* [情绪系统设计](emotion_system_plan.md)：情绪状态与已实施阶段
