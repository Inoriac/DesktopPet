# 🐾 Desktop Pet (3D桌宠)




一个基于 C++ 和 Qt 6 开发的高性能、高可定制化的桌面宠物系统。
本项目旨在提供一个生动、智能且具有“灵魂”的桌面伴侣，支持 GLTF 模型热加载、复杂的动画状态机、情绪系统以及丰富的桌面交互功能。
近期开发重心已转向 **Agent 框架**：在保留桌宠表现力的同时，引入本地意图路由、安全工具运行时、LLM 对话循环、短期记忆与外部工具接入原型，让桌宠逐步从“会动会聊”升级为“有边界的陪伴型生活助理”。

> **⚠️ 重要声明 / Important Notice**
> 
> 本项目基于 **MateEngine** 的部分核心逻辑与资源开发。
> 遵循 **MateEngine Pro License (v2.1)** 协议。
> **严禁商用**。仅供学习、研究与非商业用途。
> This project is strictly **Non-Commercial**.

---

## ✨ 功能特性 (Features)

### ✅ 已实现 / Implemented

* **核心引擎**
  * **GLTF 模型热加载**：支持运行时动态加载 `.gltf` 模型文件。
  * **动画状态机 (ASM)**：基于状态机的复杂动画调度系统，支持平滑插值过渡。
  * **快照混合过渡 (Snapshot-based Blending)**：重构了 `AnimationCrossfader`，采用骨骼快照插值解决动作切换时的物理抖动，屏蔽了首帧关键帧时间偏移导致的诡异形变，同时使用帧 `deltaTime` 上限修补了系统阻塞可能导致的立刻跳关键帧闪现问题。
  * **性能优化**：支持帧率、渲染质量、窗口大小的多级动态设置。
* **交互系统**
  * **触摸反馈**：基于身体部位（头部、身体等）的覆盖层检测，支持不同的触摸反应。
  * **窗口互动（Windows）**：
    * 支持拖动宠物窗口与窗口顶部吸附（Window Sit）。
    * 吸附判定采用“探测区 + 目标窗口顶部条”逻辑，松手后进入吸附状态。
    * 吸附期间可持续跟随目标窗口移动，目标窗口最大化/全屏时自动退出吸附。
    * 支持活动范围限制：桌宠窗口不允许超出屏幕边界。
    * 当吸附跟随触顶且目标继续上移时，自动退出吸附并触发约 2 秒的重力加速度下落到底部。
  * **鼠标追踪（3D约束版）**：基于头部屏幕投影点与光标位置计算追踪方向，支持整体朝向与头部联动。
* **生活功能**
  * **智能闹钟**：不仅仅是提醒。宠物有一定概率（基于性格）“忘记”提醒，并在事后表现出懊恼或敷衍的反应。
  * **统计系统**：记录互动次数、情绪变化、运行时间等数据，让宠物的成长有迹可循。
* **AI / Agent 框架**
  * **OpenAI-compatible LLM 接入**：支持异步请求、失败重试、Token 统计与调用日志记录。
  * **本地意图路由 `IntentRouter`**：时间查询、简单问候、LX Music 播放控制等高频低风险指令可绕过 LLM 直接执行，降低延迟。
  * **统一工具体系**：基于 `AITool + ToolRegistry` 注册工具，支持函数 Schema 导出与参数校验。
  * **安全工具运行时 `ToolRuntime + PolicyEngine`**：按 L0-L4 风险等级执行工具，支持确认/拒绝策略、敏感字段脱敏与工具结果摘要。
  * **上下文与记忆骨架**：`ContextManager / ContextBudget / MemoryStore` 用于按需构建 LLM 上下文，并将短期回复和工具事件写入 `log/ai_memory.json`。
  * **AgentCore 原型**：已具备 `AgentSession` 状态流转、LLM 规划、工具观察与多轮工具调用循环的框架。
  * **MCP 外部工具接入原型**：包含 `McpClient`、`McpServerProcess` 与 `McpToolAdapter`，后续可将外部工具统一纳入安全运行时。
  * **已注册工具组**：桌宠动画、当前时间、文件读取/目录查看（限定根目录）、网页获取/搜索（SSRF 防护）、LX Music 播放/暂停/切歌/歌词/歌单等。
* **个性化与多配置兼容**
  * **动画表现分离**：`animation_state_machine.json` 结合 `state_machine_define.json` 管理动作表现与业务逻辑，提高定制灵活性。
  * **现代化设置界面**：主设置窗口已重构为 Dashboard 风格，采用 Hero Banner、左侧导航、右侧卡片页和页面淡入淡出切换，避免传统 `GroupBox + FormLayout` 堆叠式界面。
  * **Light / Dark 主题**：由 `ThemeManager` 统一管理，支持约 0.6 秒的整窗平滑过渡，菜单栏、状态栏、页面、卡片与边框颜色保持一致。
  * **现代控件组件**：内置自绘 `SwitchButton`、`RoundSlider`、`AnimatedComboBox` 与 `AnimatedScrollBar`。开关采用 Android WLAN 风格；角色大小、音量和透明度滑块使用圆形手柄；当前角色、说话人角色和模型选择下拉框支持三角箭头展开/收起旋转；右侧滚动条在空闲时保持细胶囊，悬停或拖动时平滑展开。
  * **配置热加载**：无需重启即可应用部分功能开关与参数调整。

### 🚧 开发中 / In Progress

* **情绪系统**：包含开心、害羞、难过、愤怒四种基础情绪，随互动衰减或增强，影响对话与动作。
* **聊天系统**：
  * 本地简易对话（基于情绪的语义映射）。
  * 基于大模型的自由对话体验、语气一致性与工具调用反馈仍在持续完善。
* **陪伴 / 生活助理 Agent**：
  * 计划引入 `AgentScheduler`，将主动触发从多个定时器升级为 `Trigger + Policy + Action` 调度模型。
  * 优先支持一次性提醒、每日提醒、固定间隔提醒、稍后提醒、勿扰时间与主动冷却。
  * 后续接入天气、节假日、电量、网络状态、用户空闲状态等低风险生活感知工具。
* **物理系统**：全局物理模拟（重力、头发飘动等）。
* **视觉增强**：
  * **特殊节日皮肤**：圣诞帽、南瓜头等节日限定饰品自动渲染。
* **多媒体**：通过 LX Music 开放 API 控制播放、切歌、歌词和歌单。
* **多实例支持**：桌面上同时存在多只宠物。

---

## 🛠️ 技术栈 (Tech Stack)

* **语言**: C++ 20
* **UI 框架**: Qt 6 (Widgets & Core，自绘控件 + QSS 主题系统)
* **构建系统**: CMake
* **图形/模型**: OpenGL, TinyGLTF
* **网络 / LLM**: Qt Network, OpenAI-compatible Chat Completion
* **音频 / 外部播放控制**: OpenAL / Qt Multimedia, LX Music API
* **可选语音合成**: Python 虚拟环境 + GENIE / `genie-tts`（GPT-SoVITS 轻量推理）

---

## 📂 项目结构 (Project Structure)

```text
Desktop-Pet/
├── assets/             # 资源文件 (模型, 动画, 配置, 图标)
├── config/             # 配置文件 (状态机定义, 个性设置)
├── core/               # 核心逻辑 (动画控制, 行为树, 事件处理)
│   └── ai/             # AI 与 Agent 框架 (LLM, Router, Tools, Memory, MCP)
├── docs/               # 架构文档与 Agent 方案草案
├── engine/             # 渲染引擎 (OpenGL封装, 模型加载, 音频)
├── entity/             # 实体逻辑 (宠物类, 个性化数据)
├── ui/                 # 界面实现 (现代设置窗口, 自绘控件, 托盘, 菜单)
├── statistic/          # 数据统计模块
├── tests/              # Qt Test 单元测试与工具测试
└── third_party/        # 第三方库 (TinyGLTF等)
```

---

## 🚀 快速开始 (Getting Started)

### 环境要求

* C++ 编译器 (支持 C++20, 如 MSVC 2019+ 或 MinGW 11+)
* Qt 6.x SDK
* CMake 3.20+

### 构建步骤

1. **克隆仓库**
   
   ```bash
   git clone https://github.com/YourUsername/Desktop-Pet.git
   cd Desktop-Pet
   ```

2. **配置 CMake**
   
   ```bash
   mkdir build
   cd build
   cmake .. -DCMAKE_PREFIX_PATH="path/to/your/Qt/6.x.x/mingw_64"
   ```

3. **编译**
   
   ```bash
   cmake --build .
   ```

4. **运行**
   确保 `assets` 文件夹与可执行文件在同一级目录，或在配置中正确指定路径。

---

## ⚙️ 配置说明 (Configuration)

项目包含两份主要配置文件：

1. **`default_common_config.json`**: 系统默认配置，用于初始化或恢复出厂设置。
2. **`userConfig.json`** (生成): 用户自定义配置，保存界面风格、功能开关等。

### 主窗口与外观

主设置窗口使用 Qt Widgets 实现现代 Dashboard 布局，主要 UI 组件位于 `ui/`：

* `ThemeManager`: 统一维护 Light / Dark 全局 QSS、窗口调色板与 Hero Banner 主题属性。
* `NavigationWidget`: 左侧导航栏，支持选中胶囊动画和页面切换。
* `CardWidget`: 设置页卡片容器，区分标题区、内容区和配置项分隔线。
* `SwitchButton`: 自绘开关控件，替代传统复选框，用于 AI、语音、置顶、鼠标穿透等启用项。
* `RoundSlider`: 自绘圆形手柄滑块，用于角色大小、音量与气泡透明度。
* `AnimatedComboBox`: 自绘下拉箭头，列表展开时三角形平滑旋转 180°，收起时自动复位。
* `AnimatedScrollBar`: Web 风格胶囊滚动条，空闲时细窄，悬停、滚动或拖动时平滑展开。

这些控件保持原有 Qt 控件 API 和信号兼容，功能逻辑仍由 `MainWindow`、`PetWindow` 和配置管理模块处理。

### AI / Agent 相关配置

`config/default_common_config.example.json` 提供安全示例；实际密钥请放入本地配置，不建议提交到仓库。

`aiSettings.profiles.<profile>` 主要字段：

* `enabled`: 是否启用 AI。
* `provider`: 当前为 `openai-compatible`。
* `baseUrl`: OpenAI-compatible API 地址，例如 `https://api.example.com/v1`。
* `apiKey`: API Key，仅用于本地配置。
* `model`: 文本对话模型。
* `visual_model`: 视觉模型预留字段。
* `timeoutMs` / `maxTokens` / `temperature` / `retryCount`: 请求超时、生成长度、温度和重试次数。
* `screenChat`: 屏幕观察/主动气泡聊天相关配置，默认关闭。
* `voice`: 可选 Python / GENIE 语音合成配置，默认关闭。
* `behaviorPolicy`: Agent 主动行为白名单、禁止动作与触发间隔配置。

当前主动触发策略包括：

* `idleAction`: 空闲动作触发。
* `emotion`: 情绪表现触发。
* `proactiveChat`: 主动聊天触发。

主动触发会受白名单和禁止动作限制，避免 LLM 直接触发拖拽、窗口吸附等应由本地交互管线管理的动作。

### 可选语音合成（GENIE / GPT-SoVITS）

语音功能默认不启动。启用后，桌宠在展示 AI 回复气泡时，会把同一段文本交给 Python 侧 `genie-tts` worker 播放。

1. 准备 Python 虚拟环境：

  ```powershell
  .\tools\voice\setup_voice_env.ps1
  ```

2. 下载 GENIE 基础资源和预设说话人，例如中文菲比：

  ```powershell
  .\.venv\Scripts\python.exe tools\voice\download_genie_assets.py --preset feibi --with-roberta
  ```

  也可以一次准备全部内置预设：

  ```powershell
  .\.venv\Scripts\python.exe tools\voice\download_genie_assets.py --all --with-roberta
  ```

3. 在 UI 中勾选 `Voice synthesis (GENIE / Python)`，并选择预设角色：
  * `feibi`: 中文
  * `mika`: 日语
  * `thirtyseven`: 英语

4. 自定义角色请先按 GENIE 文档将 GPT-SoVITS 模型转换为 ONNX，然后放入：

  ```text
  runtime/voice/custom_characters/<角色名>/tts_models/
  ```

  并在 `aiSettings.profiles.<profile>.voice.customSpeaker` 中配置 `name`、`language`、`onnxModelDir`、可选 `referenceAudioPath` 与 `referenceAudioText`，再在 UI 中选择“自定义角色”。

语音运行文件位于 `runtime/voice/`，虚拟环境位于 `.venv/`，这些本地资源默认不会提交到 Git。

### 窗口吸附相关配置

在 `config/default_common_config.json` 的 `interactionSettings.windowSnapping` 下可配置：

* `enabled`: 是否启用窗口吸附（预留总开关）。
* `snapThreshold`: 吸附判定阈值。
* `verticalOffset`: 吸附时的垂直偏移。
* `snapZoneOffset`: 探测区偏移。
* `snapZoneSize`: 探测区尺寸。
* `followIntervalMs`: 吸附跟随刷新间隔。
* `forceExitOnBigScreenAlarm`: 触发大屏报警时是否强制退出吸附。
* `totalWindowSitAnimations`: Window Sit 动画变体数量配置（当前保留参数，便于后续动作验证）。

### 吸附行为规则（当前实现）

* 拖拽中仅做预吸附判定，不立即切换到 `WindowSit`。
* 鼠标松开后若命中吸附目标，才触发 `window_sit`。
* 退出吸附时触发 `window_stand`，恢复为普通状态。
* 拖拽开始优先触发 `start_drag`；若状态机未成功切入 `Drag`，会做兜底切换。
* 窗口层级切换使用运行时方式（避免通过重建窗口导致闪烁/消失）。

---

## 🧠 Agent 框架说明

当前 AI 入口仍以 `AIBrain` 为主，新增 Agent 模块作为后续演进骨架。整体思路是：**简单任务本地快速执行，复杂任务才进入 LLM；所有工具调用统一经过安全策略。**

### 当前调用链

```text
用户输入 / 定时事件
  -> IntentRouter 本地意图路由
  -> DirectReply / DirectToolCall / NeedLLM
  -> ToolRuntime + PolicyEngine
  -> AITool / ToolRegistry
  -> LLM 观察结果并继续或结束
```

### 核心目录

```text
core/ai/
├── ai_brain.*              # 当前 AI 入口，负责主动触发、LLM 循环和工具调度
├── agent/                  # AgentCore、AgentSession、AgentState
├── router/                 # IntentRouter 与路由结果类型
├── tools/runtime/          # ToolRuntime、PolicyEngine、结果脱敏
├── tools/                  # 动画、环境、文件、网络、LX Music 工具
├── context/                # ContextManager 与上下文预算
├── memory/                 # MemoryStore 与记忆类型
├── mcp/                    # MCP Client / Server Process / Tool Adapter 原型
└── llm/                    # OpenAI-compatible LLM Client 与异步 ChatService
```

### 安全边界

工具按风险等级处理：

| 等级 | 类型 | 当前策略 |
|---|---|---|
| L0 | 安全只读查询，如当前时间 | 允许 |
| L1 | 本地查询，如文件读取、目录查看 | 需限制访问根目录 |
| L2 | 低风险动作，如播放动画、切歌 | 允许或按配置限制 |
| L3 | 高风险动作，如 Shell、写文件、启动程序 | 需要用户确认 |
| L4 | 危险动作，如删除文件、读取密码/密钥 | 默认拒绝 |

工具结果会通过 `ToolResultSanitizer` 脱敏，`api_key`、`token`、`password`、`secret`、`credential`、`authorization` 等字段不会直接进入 LLM 上下文。

### 相关日志与测试

* `log/ai_calls.jsonl`: LLM 请求与响应日志。
* `log/ai_memory.json`: 短期回复和工具事件记忆。
* `tool_tests`: 工具注册、策略、脱敏等测试。
* `llm_tests`: 异步 LLM 请求与重试测试。
* `file_web_tool_tests`: 文件工具、网络工具与安全校验测试。

### 当前规划文档

* `docs/agent_architecture.md`: Agent 总体架构草案。
* `docs/companion_life_assistant_agent_plan.md`: 陪伴型与生活助理型 Agent 路线。

---

## 🎯 光标追踪行为说明 (Cursor Tracking Behavior)

当前实现遵循以下约束：

* 以**屏幕坐标系**为参考（将屏幕视作 `xoy` 平面）。
* 桌宠整体保持**正立**，不进行明显的躯干倾斜。
* 主要表现为模型**整体绕 `Y` 轴**进行“向内/向外”转向，而不是平行于屏幕平面的倾倒。
* 头部进行小范围补偿，当前限制为：`Yaw = 20°`，`Pitch = 14°`。
* 躯干骨骼仅保留较小幅度的辅助 yaw，避免视觉上“全身平面扭转”。

实现要点：

* 在渲染阶段通过头部骨骼投影到屏幕，计算“头部屏幕位置 -> 光标”方向向量。
* 将该向量用于：
  * 模型整体 `Y` 轴旋转（主导）
  * 头部与眼部骨骼跟随（次级）
* 对模型轴向做了旋转顺序修正：先应用整体 `Y` 轴追踪旋转，再进行模型朝向扶正，减少轴向错位导致的错误观感。

> 说明：当前追踪参数主要在代码中定义（如 `core/animation/animation_player.cpp`、`engine/render_engine.cpp`），后续可按需要外置到配置文件。

---

## 📄 协议与版权 (License & Credits)

### Project License

本项目遵循 **MateEngine Pro License (v2.1)**。
查看完整的 [LICENSE](LICENSE.md) 文件。

### Attribution

* **Developer**: Inoriac
* **Original Engine Logic & Inspiration**: [Mate-Engine](https://github.com/shinyflvre/Mate-Engine) by Johnson Jason.
* **Assets (Animations/Visuals)**: Copyright © 2025 **Shiny**.
  * *Note: Assets are used under a non-commercial license and strictly copyrighted.*

**严禁将本项目用于任何商业用途（包括但不限于 Steam、itch.io 销售或付费订阅）。**
**Strictly prohibited for commercial use.**
