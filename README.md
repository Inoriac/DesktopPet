# 🐾 Desktop Pet (3D 桌宠)

一个基于 C++20 / Qt 6 的高性能桌面宠物，并在其上构建一套有安全边界的 Agent 框架。
目标是构建具有持续身份、人物关系和记忆，能够自主选择活动并通过插件获得新能力的 Agent；桌宠是它的表现与交互形式。现有实现包含 3D 表现、本地意图路由、工具运行时、LLM 对话循环、记忆系统、自主活动与通用插件运行时。

> **⚠️ 本项目基于 MateEngine 部分核心逻辑与资源开发，遵循 MateEngine Pro License (v2.1)，严禁商用，仅供学习与研究。**

**第一次使用？** 先看[安装与启动](#安装启动指南)，遇到 DLL 问题看 [Windows 运行库排查](#windows-dll)，然后按需启用[内置插件](#内置插件上手)和 [QQ / NapCat](#qq-后台插件可选)。最后用[完整功能验收清单](#完整功能验收)逐项确认。

本项目不是下载一个 EXE 就具备全部能力的成品安装包：**C++ 核心负责桌宠与 Agent，Python launcher 是正式入口，插件及外部服务需要逐个配置**。3D 角色资产需自行准备；LLM 需要可用的模型服务；QQ 需要另行运行 NapCat 或 SnowLuma。不需要的能力可以保持关闭，不影响基础使用。

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

* **Embedding 分发与兼容性**：仓库已包含 `assets/embeddings/model_quantized.onnx` 与词表；平台专属 ONNX Runtime 仍需单独准备。模型或运行库不可用时语义检索会降级为关键词路径，真实数据效果和跨平台分发仍需校准。
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

<a id="安装启动指南"></a>

## 🚀 安装与使用

以下流程以 Windows 10 / 11 为主。当前已实机验证的组合是 Qt 6.5.3 MinGW 64-bit、MinGW 11.2、CMake/Ninja 和 Python 3.12；MSVC 2019+ 也受构建脚本支持，但 Qt 套件必须与编译器 ABI 匹配，不能混用 MinGW Qt 与 MSVC。

### 1. 运行前置

* Git、CMake 3.20+、Ninja 或 Visual Studio 生成器，以及支持 C++20 的编译器。
* Qt 6，包含 Core、Gui、Widgets、Network、OpenGL、OpenGLWidgets、Sql 和 Test 模块。
* 推荐 **Python 3.12 x64**，使用 3.10–3.12 可同时覆盖 launcher 与当前插件。不要直接选最新 Python：launcher 固定了 PySide6-Essentials 6.7.3，较新 Python 可能没有对应 wheel。
* 支持 OpenGL 3.3 Core 的显卡和驱动。
* 可选：libsodium 与 Qt6Keychain 同时存在时启用加密内心活动和私人日记；缺失时普通桌宠仍可构建，但不会降级保存明文私密内容。
* MSVC 构建或使用由 MSVC 编译的第三方 DLL / Python 扩展时，安装微软官方 [Visual C++ v14 x64 运行库](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)。CMake 不会替你执行系统安装程序。Debug 程序需要开发环境，不作为普通用户分发版本。

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

只想先启动、不构建全部测试时，最后一条可以改为 `cmake --build build --target Desktop_Pet desktop_pet_web --parallel`。测试前再执行完整构建。**编译 Qt/C++ 核心不依赖 Python 的 PySide6；安装 PySide6 也不能替代原生 Qt 开发套件。**

使用 Visual Studio 2022 和匹配的 Qt/MSVC 套件时，可改用：

```powershell
cmake -S . -B cmake-build-msvc -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH='C:\Qt\6.8.3\msvc2022_64'
cmake --build cmake-build-msvc --config Release --parallel
$env:DESKTOP_PET_EXECUTABLE = (Resolve-Path 'cmake-build-msvc\Release\Desktop_Pet.exe').Path
```

两个方案二选一；切换编译器、Qt 套件、架构或生成器时使用新的构建目录，不复用旧 CMakeCache。MSVC 示例用了自定义目录，所以每次新开终端启动 launcher 前都要设置上述 `DESKTOP_PET_EXECUTABLE`。后文 `build` 路径相应改为 `cmake-build-msvc`，所有 `cmake --build` 命令加 `--config Release`，所有 `ctest` 命令加 `-C Release`。

每次拉取包含 CMake 或测试注册变化的代码后，先重新执行原来的 `cmake -S . -B ...` 配置命令。当前 Windows 构建默认为**所有可执行目标**运行统一部署，包括核心、Web helper、单独构建的测试和 fixture；使用 toolchain 文件时也不会跳过。具体配置按实际构建的 Debug / Release 选择，不混用 DLL。

<a id="windows-dll"></a>

### Windows DLL 自动部署与排查

链接完成后 CMake 调用同一 Qt 套件的 `windeployqt`，部署 Qt DLL、平台 / SQLite / TLS 等插件和编译器运行库，再扫描导入依赖、补齐能找到的非系统 DLL（如 libsodium、QtKeychain、ONNX Runtime）。同一输出目录的部署串行执行，避免并行构建相互覆盖。找不到工具或依赖会明确报错，不再静默产出缺库程序。

这解决的是**链接完成后或运行测试时缺运行库**的问题，不会自动安装未安装的 SDK，也不能修复不同 ABI / 架构的库。`LNK1104`、`cannot find -l...` 是链接阶段缺库；`0xc0000135`、缺 `Qt6Core.dll` 等通常是启动阶段缺库，应区分处理。

| 现象 | 处理方式 |
| --- | --- |
| 找不到 `windeployqt` | 检查选中的 Qt 套件是否包含部署工具；必要时配置 `-DDESKTOP_PET_WINDEPLOYQT=C:/Qt/对应套件/bin/windeployqt.exe`，不能借用另一版本或 PySide6 内的 Qt DLL |
| 缺 `Qt6*.dll`、`libstdc++-6.dll`、`libgcc_s_*.dll`、`libwinpthread-1.dll` | 先重新配置并构建该目标；核对编译器 / Qt 的位数和版本，以及 MinGW `bin` 是否存在。保留输出目录全部运行库，不要只移动 EXE |
| 缺 `libsodium.dll`、`qt6keychain*.dll`、ONNX 或其他第三方 DLL | 提供对应 DLL 的目录；只有 `.lib` / `.dll.a` 导入库并不足以运行，第三方运行库也必须匹配架构和配置 |
| `qwindows` 平台插件失败、`QSQLITE driver not loaded` | 保留 EXE 旁的 `platforms` / `sqldrivers`；排查全局 `QT_PLUGIN_PATH` / `QT_QPA_PLATFORM_PLUGIN_PATH` 是否指向别的 Qt。可临时设 `$env:QT_DEBUG_PLUGINS='1'` 查看日志，结束后移除 |
| `0xc000007b`、入口点不存在 | 常见于 x86/x64、Debug/Release 或 Qt 版本混装；使用新构建目录和同一套依赖，不从网上随意补同名 DLL |
| 拷贝 DLL 失败 / access denied | 先退出桌宠、测试和 Web helper，再重建；检查目标目录写权限及安全软件拦截 |

第三方运行库不在标准位置时，将实际目录传入 CMake；可以用分号分隔多个目录，Debug 和 Release 应分别配置匹配的路径：

```powershell
cmake -S . -B build '-DDESKTOP_PET_RUNTIME_SEARCH_DIRS=C:/deps/bin;C:/other-deps/bin'
cmake --build build --target Desktop_Pet desktop_pet_web --parallel

# EXE 没变、只是 DLL 被误删时，不必改源码强制重新链接：
cmake --build build --target Desktop_Pet_deploy_runtime desktop_pet_web_deploy_runtime
# 单独测试也有对应目标，例如 qq_plugin_tests_deploy_runtime。
```

部署开关 `DESKTOP_PET_DEPLOY_WINDOWS_RUNTIME` 默认 ON。仅在你已有独立打包流程或交叉编译时设为 OFF，并自行部署依赖。发布给没有开发环境的机器时使用 Release，保留整个部署目录，安装所需微软运行库，再在干净终端验证；本项目仍需要源码资源目录、Python launcher、模型与各插件状态目录，**部署 DLL 不等于制作了完整安装包**。

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

1. 首次打开 launcher 会创建默认角色 `Milltina` 和稳定的随机 `profileId`。当前界面可以选择已有角色，尚不提供新增角色流程。如需修改 `pets.json` 的 `modelPath`，先关闭 launcher 和桌宠，保留 `profileId`，保存 UTF-8 JSON 后重新打开；GLTF 的外部纹理与 `.bin` 文件也要齐全。
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
| 语义记忆检索 | 平台专属 ONNX Runtime；仓库已带 embedding 模型 / 词表 | [增强能力准备](#增强能力准备) |
| 加密内心活动 / 私人日记 | 构建时同时找到 libsodium 与 Qt6Keychain；运行时密钥链可用、日记模型配置正确 | [增强能力准备](#增强能力准备) |

插件安装不会替用户打开宿主自主活动总开关。先退出目标角色，预览配置，确认路径和授权后再应用，随后重启角色。模型是否自主使用某个能力还取决于插件授权、全局开关、上下文和预算。

### 7. 基础验证

```powershell
# Ninja 单配置构建
ctest --test-dir build --output-on-failure -j 4

# Visual Studio 多配置构建使用：
# ctest --test-dir build -C Release --output-on-failure -j 4

& .\.venv\Scripts\python.exe -m unittest discover -s tests -p 'test_launcher_*.py'
```

Qt / DLL 缺失属于环境失败，不应当作正常跳过。测试出现 `Skipped` 时先确认该测试是否缺少 Python 包或可选后端；跳过不能记作该集成已经通过。完整 launcher 说明见 [launcher/README.md](launcher/README.md)，配置结构见 [config/default_common_config.example.json](config/default_common_config.example.json)。

<a id="内置插件上手"></a>

## 内置插件：从安装到实际使用

“内置”表示能力包源码随仓库提供，**不表示已经安装 Python 依赖、登录平台或获得角色权限**。Web / B 站 / LX Music / QQ 均通过角色目录里的 `plugins.json` 接入；没有统一的一键全开按钮。以下命令从仓库根目录执行，不需要手动启动插件适配器或 source 常驻进程，宿主会按需管理它们。

### 共用准备

先通过 launcher 启动目标角色一次，再退出桌宠。每开一个新 PowerShell 窗口，先重新设置变量：

```powershell
$petPython = (Resolve-Path '.venv\Scripts\python.exe').Path
$petDataDir = Join-Path $env:APPDATA 'Desktop Pet Team\Desktop Pet'
$pets = Get-Content -LiteralPath (Join-Path $petDataDir 'pets.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$pets | Select-Object name, profileId
$profileId = '替换为目标角色的真实小写UUID'
$profileDir = Join-Path $petDataDir "profiles\$profileId"
if (!(Test-Path -LiteralPath $profileDir -PathType Container)) { throw '请先启动该角色一次再退出' }
$pluginState = Join-Path $env:LOCALAPPDATA "DesktopPet\plugins\$profileId"
$OutputEncoding = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
```

每项按“准备依赖 → 预览 → `--apply` → 重启角色 → 真实只读调用”操作。只有 `--apply` 才写入角色配置；保留输出的 `backupDirectory`。所有路径都是绝对路径，移动仓库、构建目录或 venv 后须重新配置。以下示例显式使用 `--autonomous off`，先验证手动调用；Web / B 站 / QQ 配置器省略此参数时默认是 on，不要误以为省略等于关闭。

### Web：搜索与网页阅读

无需 API Key 或额外 Python 包，只需已经构建的 Qt helper：

```powershell
$webProgram = (Resolve-Path 'build\desktop_pet_web.exe').Path
# MSVC 示例改用 cmake-build-msvc\Release\desktop_pet_web.exe。
$webState = Join-Path $pluginState 'web'
$webArgs = @('-B', 'plugins\web\configure.py', '--profile-dir', $profileDir,
  '--program', $webProgram, '--state-dir', $webState, '--autonomous', 'off')
& $petPython @webArgs
if ($LASTEXITCODE -ne 0) { throw 'Web 配置预览失败' }
& $petPython @webArgs --apply
if ($LASTEXITCODE -ne 0) { throw 'Web 配置失败' }
```

重启后可以说：“用 `desktop-pet.web` 搜索 Qt 官方文档，读取一个结果并附上来源。”核对真实搜索结果和正文，不以模型声称“已浏览”为准。搜索使用公开 Bing RSS，可能限流或不可达；网页读取不执行 JavaScript、不带登录 Cookie、不解析 PDF，也不能访问内网 / localhost。遇到登录页或正文为空不等于安装失败。详情见 [Web 指南](plugins/web/README.md)。

### Bilibili：搜索、字幕与可选转写

先准备独立环境，再由人类登录；不要把 Cookie 贴到聊天中：

```powershell
$biliState = Join-Path $pluginState 'bilibili'
& $petPython -B plugins\bilibili\setup.py --state-dir $biliState
if ($LASTEXITCODE -ne 0) { throw 'B 站环境安装失败' }
$biliPython = Join-Path $biliState 'venv\Scripts\python.exe'
& $biliPython -B plugins\bilibili\login.py --state-dir $biliState
# 按输出路径打开本地二维码，用 B 站 App 扫码；成功后再继续。
if ($LASTEXITCODE -ne 0) { throw 'B 站登录未完成' }
$biliArgs = @('-B', 'plugins\bilibili\configure.py', '--profile-dir', $profileDir,
  '--state-dir', $biliState, '--autonomous', 'off')
& $biliPython @biliArgs
if ($LASTEXITCODE -ne 0) { throw 'B 站配置预览失败' }
& $biliPython @biliArgs --apply
if ($LASTEXITCODE -ne 0) { throw 'B 站配置失败' }
```

公开搜索可尝试匿名，但字幕通常要求登录；没有字幕并不代表已经有本地转写能力。要启用 ASR，另行下载模型并重新授予启动转写权限：

```powershell
& $petPython -B plugins\bilibili\setup.py --state-dir $biliState --asr-model tiny
if ($LASTEXITCODE -ne 0) { throw 'ASR 模型准备失败' }
$biliModel = Join-Path $biliState 'models\tiny'
& $biliPython @biliArgs --asr-model-path $biliModel
if ($LASTEXITCODE -ne 0) { throw 'ASR 配置预览失败' }
& $biliPython @biliArgs --asr-model-path $biliModel --apply
if ($LASTEXITCODE -ne 0) { throw 'ASR 配置失败' }
```

重启后试：“用 B 站插件搜索一个科普视频，读取字幕，区分原字幕与本地转写并注明时间戳。”插件只读，不发评论、不点赞，也不理解画面。ASR 使用 CPU/int8，`tiny` 适合连通性测试，中文准确率不保证；`accepted` 是任务已受理，需继续查询到完成。后续 configure 不传 `--asr-model-path` 会关闭 ASR 授权。平台风控、登录过期、无字幕和下载失败不能靠 CMake 解决，详见 [B 站指南](plugins/bilibili/README.md)。

### LX Music：连接真实播放器

安装并打开 Windows LX Music，在设置中启用本地开放 API，核对端口（示例 `23330`）；先手动确认播放器能播放。此插件使用 Python 标准库，不安装额外 pip 包，也不提供音乐源。

```powershell
$musicDatabase = Join-Path $env:APPDATA 'lx-music-desktop\LxDatas\lx.data.db'
$musicExecutable = 'C:\替换为实际安装目录\lx-music-desktop.exe'
# 便携版 / 不同版本请改成实际数据库路径，不要创建一个空数据库充数。
$musicArgs = @('-B', 'plugins\lx-music\configure.py', '--profile-dir', $profileDir,
  '--database', $musicDatabase, '--executable', $musicExecutable,
  '--api-url', 'http://127.0.0.1:23330', '--autonomous', 'off')
& $petPython @musicArgs
if ($LASTEXITCODE -ne 0) { throw '音乐配置预览失败' }
& $petPython @musicArgs --apply
if ($LASTEXITCODE -ne 0) { throw '音乐配置失败' }
& $petPython -B plugins\lx-music\adapter.py `
  --settings (Join-Path $profileDir 'lx-music.settings.json') --diagnose
```

诊断的 `state=completed` 只表示诊断执行完毕，实际看 `data.availability` 和 API / 数据库 / 程序路径各项。重启后试：“用 `desktop-pet.lx-music` 查询当前曲目和歌单，先不要播放。”旧原生 `lx_music_*` 工具仍存在，不算新插件接入成功。只读通过后再试播放 / 暂停；`unknown` 代表可能已执行，先查状态，不能自动重发。自主权限可选 `off/read/all`，建议先 read。详情和回滚见 [LX Music 指南](plugins/lx-music/README.md)。

### 自主使用、停用与隐私

* 插件授权和 launcher 的“自主活动 / 暂停自主活动”是两道独立开关。启用两者也不保证立即执行；主体可以等待，预算和空闲判断仍然有效。
* 在角色停止时重新 configure 可以调整授权；`--autonomous off` **只撤销自主权限，不禁用本地主人的插件调用**。彻底停用时，备份 `plugins.json`，将对应 `plugins` 项的 `enabled` 改为 `false`，再重启；不要删除整个配置或记忆数据库。
* QQ 消息、网页、字幕等材料可能进入模型上下文；是否出机取决于连接档案所指向的模型服务。“本地插件”不等于“材料仅在本地处理”。只授权自己有权读取的内容，并征得会话参与者同意。
* token、B 站凭证、QQ 消息缓存及角色数据库都可能含敏感内容。保存在当前账户可控目录，不提交 Git、不随日志打包公开；回滚整份旧 `plugins.json` 会同时撤销其后的其他插件修改。

<a id="增强能力准备"></a>

## 语音、语义记忆与私人日记

**语音合成（不是语音识别）：** 默认关闭。仓库根目录运行以下命令准备独立语音环境与预设，下载会占用磁盘 / 网络，不在普通聊天时自动安装：

```powershell
powershell -ExecutionPolicy Bypass -File tools\voice\setup_voice_env.ps1 -VenvPath runtime\voice\venv
& .\runtime\voice\venv\Scripts\python.exe tools\voice\download_genie_assets.py --preset feibi --with-roberta
if ($LASTEXITCODE -ne 0) { throw '语音资源验证失败，请勿先开启语音' }
```

关闭 launcher 和桌宠，在本机 `launch_config.json` 的现有 `voice` 对象中将 `venvPath` 改为 `runtime/voice/venv`（也可用绝对路径），保留其他字段；具体嵌套位置参照[默认模板](config/default_common_config.example.json)。重新打开 launcher，在“语音”页选择已下载的 `feibi` 并开启合成。自定义音色还需要转换后的模型与参考音频，不能只换一个名字。若共用根目录 `.venv` 可使用脚本默认路径，但可能改变 launcher 的 Python 依赖；独立环境更稳妥。

**语义记忆检索：** 仓库已跟踪约 55 MiB 的量化模型和词表，位于 `assets/embeddings`；需要另外准备 **C++ ONNX Runtime**，Python `pip install onnxruntime` 不能代替它：

```powershell
powershell -ExecutionPolicy Bypass -File tools\fetch_onnxruntime.ps1
if ($LASTEXITCODE -ne 0) { throw 'ONNX Runtime 准备失败' }
cmake -S . -B build -DDESKTOP_PET_ENABLE_ONNX_RUNTIME=ON
cmake --build build --target Desktop_Pet memory_strategy_tests --parallel
ctest --test-dir build -R '^MemoryStrategyTests$' --output-on-failure
```

配置输出应包含 `onnxruntime found`，Windows 包须同时包含头文件、链接用 `onnxruntime.lib` 和运行用 DLL；重新编译后才启用。下载脚本固定版本若无法获取，按 [ONNX Runtime 说明](third_party/onnxruntime/README.md)核实官方可用版本与平台。只在此目录保留当前平台的候选包，避免误选另一平台。暂不需要时配置 `-DDESKTOP_PET_ENABLE_ONNX_RUNTIME=OFF`，基础记忆与关键词召回仍可使用。

**加密内心活动 / 私人日记：** 安装与原生 Qt / 编译器匹配的 libsodium 和 Qt6Keychain 开发包及其运行库，再重新配置。可通过 `CMAKE_PREFIX_PATH`、`SODIUM_INCLUDE_DIR`、`SODIUM_LIBRARY`、`Qt6Keychain_DIR` 指定位置；非标准 DLL 路径用 `DESKTOP_PET_RUNTIME_SEARCH_DIRS`。配置输出必须是 `Private reflection capability available`，否则此能力没有编入。运行时还需系统凭据库可用，配置日记 / 反思模型并产生足够经历和睡眠周期；空日记列表不一定是故障。通过 launcher 的只读日记页访问，不直接编辑加密数据库。缺依赖不会降级为明文；私密正文仍可能发送给你配置的相应模型服务商。

依赖来自 [libsodium](https://doc.libsodium.org/installation) 和 [QtKeychain](https://github.com/frankosterfeld/qtkeychain)。QtKeychain 必须选择 Qt 6 构建（`BUILD_WITH_QT6=ON`），不是 Qt 5 版本；使用包管理器时也不能把其另一套 Qt 与当前 `$qtRoot` 混用。假设二者已经安装到各自包含 `include` / `lib` / `bin` 的前缀，MinGW 示例可这样接入（改成真实路径）：

```powershell
cmake -S . -B build `
  "-DCMAKE_PREFIX_PATH=$qtRoot;C:/deps/qtkeychain-install;C:/deps/libsodium" `
  '-DDESKTOP_PET_RUNTIME_SEARCH_DIRS=C:/deps/qtkeychain-install/bin;C:/deps/libsodium/bin'
cmake --build build --target Desktop_Pet --parallel
```

这条命令只查找已有依赖，不会下载它们。请以 CMake 的能力检测输出为准，而不是仅凭 configure 命令退出成功判断日记已启用。

## QQ 后台插件（可选）

**QQ 插件需要 NapCat 吗？需要一个兼容后端，但不限定 NapCat：当前明确支持 `napcat` 和 `snowluma` 两种后端。** 普通 QQ 客户端本身不能直接连接这个插件，也不是安装 `websockets` 就完成了 QQ 接入。不要把“支持 OneBot”理解为任意 OneBot 实现都已经兼容历史与发送回执。

QQ 插件通过 OneBot v11 **正向 WebSocket** 连接 NapCat 或 SnowLuma。NapCat/SnowLuma 在已经登录 QQ 的环境中提供 WebSocket 服务端，Desktop Pet 是客户端；插件不安装后端、不负责扫码登录，也不控制 QQ 窗口。无需另装 MaiBot，也不启动另一套 bot / LLM 回复循环。

```text
你登录的 QQ 账号
    ↕
NapCat 或 SnowLuma（OneBot v11 正向 WS 服务端）
    ↕ ws://127.0.0.1:3001（仅示例，实际端口由你设置）
QQ 插件（Python 适配器 / 可选消息 source）
    ↕ 角色授权与宿主活动运行时
Desktop Pet Agent（复用 launcher 配置的模型）
```

当前支持授权私聊/群聊的文本读取、文本或引用发送、发送回执，以及可选的后台消息观察。图片、语音、视频等媒体只保留占位，不支持理解或发送；断线期间的历史能否补齐取决于后端。

### 1. QQ 前置

* 已登录测试 QQ 的 NapCat 或 SnowLuma，并开启 OneBot v11 **正向 WS 服务端**。
* 后端类型、WS 地址、登录 QQ 号 `selfId`、稳定的 `connectionKey`。
* 明确允许的测试群 `group:<群号>`、测试私聊 `private:<对方QQ号>`，以及用于确认拒绝行为的未授权会话。
* 后端启用鉴权时准备非空的可打印 ASCII token。token 单独放在仓库外的文本文件中，不写入 URL、Git 或验收日志。
* 已通过 launcher 启动过一次目标角色，使 `%APPDATA%\Desktop Pet Team\Desktop Pet\profiles\<profileId>` 存在。

NapCat 与 SnowLuma 的历史接口和发送证据不同，需要分别验收，不能用其中一个的结果代替另一个。

### NapCat 具体准备顺序

1. 从 [NapCatQQ 官方仓库](https://github.com/NapNeko/NapCatQQ)及其文档 / Releases 获取适合你的 Windows 和 QQ 客户端版本的安装方式。NapCat 的安装、QQ 版本兼容和登录方式由该项目维护，不使用来源不明的整合包。
2. 启动 NapCat，按它的提示登录你有权使用的测试 QQ 账号，确认在线并可在真实 QQ 客户端收发消息。记录此账号 QQ 号作为 `selfId`，**不是**机器人的昵称、群号或对方 QQ 号。
3. 在 NapCat 网络配置中新增并启用 **OneBot v11 WebSocket 服务端 / 正向 WS**。同机使用时绑定 `127.0.0.1`，选择一个空闲端口（例如 `3001`），设置独立的强 token，保存配置，按后端要求重启服务。
4. 记录实际 WS 地址。NapCat WebUI 的管理地址、HTTP API 地址、反向 WS 客户端配置都不是这里的 `wsUrl`；若后端设置了路径，地址也必须带该路径。插件从本机发起连接，无需你再启动一个 WS 接收服务器。
5. 在后端保持在线时，完成下面的 Python 设置及只读 `status` 验证，再启动桌宠。以后启动顺序同样为 **NapCat → QQ 登录在线 → launcher → 桌宠**；QQ 掉线后桌宠可能仍能普通聊天，但 QQ 功能不可用。

| 配置项 | 本项目示例 | 注意 |
| --- | --- | --- |
| 网络类型 | OneBot v11 正向 WebSocket | 不选 HTTP，不选反向 WebSocket |
| 监听地址 / 端口 | `127.0.0.1:3001` | 只是示例，不是承诺的 NapCat 默认值 |
| token | 后端设置的访问 token | 文件中只写 token 本身；插件自动添加 `Authorization: Bearer ...`，不写进 URL |
| 消息格式 | 消息段数组优先 | 插件也能解析 CQ 字符串，但媒体只显示占位 |
| 白名单 | 一个测试群或测试私聊 | 插件 `allowedConversations` 必须显式列出；不支持 `*` |

不要把 QQ 控制接口或 WebUI 暴露在公网。跨机器连接应自行配置可信网络、鉴权与 `wss`，不是直接把示例 host 换成公网 IP。第三方 QQ 自动化有账号风控、协议变化和服务条款风险，建议先用测试账号及获准的测试群；本项目不能保证账号安全、长期兼容或实时回复。

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
if ($LASTEXITCODE -ne 0) { throw 'QQ 配置预览失败' }
& $qqPython -B plugins\qq\configure.py --profile-dir $profileDir --settings $qqSettings --source off --autonomous off --apply
if ($LASTEXITCODE -ne 0) { throw 'QQ 配置失败' }
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

本地聊天可先问：“用 QQ 插件检查连接状态，列出授权会话，只读最近几条文本，不发送。”本地主人会话无 `send` 授权，要求“帮我给某人发 QQ”被拒绝是当前设计，不应通过放宽全局安全策略绕开。外部 QQ 回复发生在该 QQ 会话自己的活动中。launcher 聊天页仍只展示本地主人的对话，**不是 QQ 收件箱**。

| 状态 | 首先检查 |
| --- | --- |
| `DEPENDENCY_UNAVAILABLE` | configure 是否由 `$qqPython` 执行；该环境是否安装了 `websockets==15.0.1` |
| `NETWORK_ERROR` / 超时 | NapCat 是否运行且账号在线；端口、路径是否为正向 WS；token 是否一致；防火墙是否拦截。可用 `Test-NetConnection 127.0.0.1 -Port 3001` 检查端口，但端口通不证明 OneBot / 鉴权正确 |
| `INVALID_SETTINGS` | settings 是否 UTF-8 无 BOM；token 非空、无 BOM、只有可打印 ASCII；QQ 号为字符串，路径为绝对路径且在插件包外 |
| `ACCOUNT_MISMATCH` | `selfId` 是否等于后端当前登录 QQ 号，切号后是否误用旧 stateDir |
| `PERMISSION_DENIED` | 会话是否在精确白名单中，profileId 和 grant 是否正确；本地主人不能发送，外部会话不能跨会话读取 / 发送 |
| `status` 正常却没有后台消息 / 不回复 | source 是否 on、配置后是否重启；自主授权和总开关是否都开启且未暂停；测试消息是否来自授权会话；主体是否选择等待 |
| 历史不全 / `local_only` | 后端历史接口不支持或有限；本地缓存不是 QQ 全量历史备份 |
| 发送 `unknown` | 先检查实际 QQ 客户端，用原 `receipt_id` 核对，绝不自动重复发送 |

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

Visual Studio 多配置构建给后两条命令加 `--config Release` 和 `-C Release`，构建目录与前文保持一致。现在单独构建 `qq_plugin_tests` 也会部署 Qt 和编译器 DLL，不再依赖先构建其他测试来补库；仍有缺库时按 [Windows 运行库排查](#windows-dll)处理。缺少 `websockets` 时部分 QQ 测试会标为跳过，不能算通过。

受控回归只证明插件协议、单次派发、回执、权限和宿主链路，不代表真实 QQ 已通过。完整配置、运行边界与回滚见 [QQ 插件文档](plugins/qq/README.md)，NapCat/SnowLuma 的 A1-A11、H1-H11 实机表见 [QQ 验收交接](plugins/qq/验收交接.md)。

<a id="完整功能验收"></a>

## 完整功能启动检查表

建议从上到下验证；前一步没有通过时，不要一次开启所有自主能力增加排障难度。下表是验收标准，不是已在每个平台验证通过的声明。

| 阶段 | 通过标准 |
| --- | --- |
| 核心 / DLL | Release 构建成功；新终端中无需手工追加 Qt / MinGW 的 `bin` 即可启动已部署程序；能独立运行刚构建的一个测试目标 |
| 模型与 launcher | launcher 可打开，角色能启动并显示；托盘、拖拽、退出正常；重启后 profileId 不变 |
| AI / 视觉 | 连接测试通过；实际文本聊天收到回复；配置视觉模型后能识别一张非敏感测试图片；连接测试只证明该次模型请求，不保证所有工具 / 视觉都支持 |
| 记忆 / 日记 | 重启后历史和身份仍在；需要语义检索时确认 ORT 与模型加载成功；日记需加密依赖和实际生成条件，不能把关键词降级或空日记直接算完整通过 |
| Web / B 站 | 真实 discover/load/invoke 调用成功；能读到网页正文、真实字幕或明确的限制；ASR 须等实际任务完成 |
| LX Music | 只读诊断就绪、曲目和歌单正确；手动播放 / 暂停与真实播放器一致，不仅看退出码 |
| QQ | 正确后端、账号、白名单；只读 status/read 正常；source 能收到测试消息；需要回复时，在精确外部会话中验证一次实际发送及回执 |
| 语音 | 已下载的音色可实际出声；语音失败不应当作文本聊天失败，也不能把 B 站 ASR 当作桌宠麦克风输入 |
| 自主活动 | 仅在手动验证通过后打开对应插件授权和总开关；确认“暂停自主活动”能阻止新自主调用，允许主体选择等待 |

不需要某项能力就保持关闭，无需为了启动桌宠安装全部可选依赖。QQ 后端、B 站登录 / 限流、音乐源及模型服务由各自外部系统决定，自动化替身测试不能替代真实环境验收。

排障先看 launcher 的“运行概览 / 诊断日志”、`log/launcher` 中的进程输出及当前角色 `profiles/<profileId>/logs`。报告问题时附 Windows / Qt / 编译器 / Python 版本、构建命令、目标名、缺失 DLL 原名或插件错误码，以及脱敏后的相关日志；不要附 API Key、token、Cookie、完整 QQ 消息或私人日记。不要以删除角色数据库作为通用修复方法。

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
