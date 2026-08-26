# 🐾 Desktop Pet (3D 桌宠)

一个基于 C++20 / Qt 6 的高性能桌面宠物，并在其上构建一套有安全边界的 Agent 框架。
目标是从「会动会聊」的桌宠演进为「有边界的陪伴型生活助理」：保留 3D 表现力，同时具备本地意图路由、安全工具运行时、LLM 对话循环与人类感记忆系统。

> **⚠️ 本项目基于 MateEngine 部分核心逻辑与资源开发，遵循 MateEngine Pro License (v2.1)，严禁商用，仅供学习与研究。**

---

## ✨ 已完成

* **核心引擎**：GLTF 模型热加载、动画状态机、骨骼快照混合过渡，帧率/渲染质量多级调节。
* **桌面交互**：基于身体部位的触摸反馈；Windows 窗口拖动与顶部吸附跟随；3D 约束版的鼠标追踪（整体绕 Y 轴转向 + 头部小范围补偿，保持正立）。
* **生活功能**：会“偶尔忘记”并事后露怯的智能闹钟；互动 / 情绪 / 运行时统计。
* **Agent 框架**
  * **LLM 接入**：OpenAI-compatible，异步请求 + 失败重试 + Token 统计 + 调用日志。
  * **多模型角色路由**：对话、快速提取、记忆整理、日记、视觉可分别配置模型；支持有序 fallback、单次结构修复和进程内短暂熔断。
  * **上下文权限投影**：按模型角色限制可见上下文；普通对话不会获得私人日记、内心活动或 owner access。
  * **本地意图路由**：时间 / 问候 / LX Music 控制等高频低风险指令绕过 LLM 直接执行。
  * **统一工具体系**：`AITool + ToolRegistry`，函数 Schema 导出与参数校验。
  * **安全运行时**：`ToolRuntime + PolicyEngine`，按 L0–L4 风险等级处置，敏感字段脱敏，文件/命令走白名单 + 安全根目录，禁止解释器绕过。
  * **上下文管理**：ContextManager 与上下文预算。
  * **记忆系统**：SQLite 主存储 + 关系图谱 + 标签共现图 + 遗忘曲线 + 情感增强 + 巩固反馈 + Working Memory 缓存；支持 5 分区自适应遗忘、遗忘扫描和可插拔向量索引。
  * **Daydream 记忆整理**：在满足空闲 / 睡眠条件时对待处理记忆做分类、合并、更新或归档；提交前使用 staged changes，中断可回滚，重启可恢复已提交会话。
  * **AgentCore 原型**：Agent 会话状态流转 + LLM 规划 + 多轮工具调用。
  * **MCP 接入原型**：客户端 / 进程管理 / 工具适配器。
  * **已注册工具**：气泡 / 通知、动画、时间、用户空闲、电池、网络、提醒任务、安全文件读写与目录查看、受控白名单命令（默认关）、网页获取 / 搜索（SSRF 防护）、天气 / 节假日 / 每日简报、LX Music 播控。
  * **技能学习**：可创建、更新、匹配和删除可复用技能，并记录技能执行结果。
* **角色身份与运行时**：每个角色使用稳定 `profileId` 隔离记忆、身份状态和私有数据；支持旧数据迁移、事件账本、outbox、consumer checkpoint、会话快照和能力降级。
* **人格成长**：基础人格与当前情绪共同投影到对话；支持人格证据、版本化人格、owner 关系、自我叙事和追加式回滚，避免一次对话直接改写角色设定。
* **私密反思与日记**：高价值事件可生成加密的简短内心活动；睡眠周期生成每日私人日记。正文使用 XChaCha20-Poly1305 加密，密钥保存在系统 Keychain，缺少私有依赖时不降级写明文。
* **Python 启动器**：原 Qt 控制面板已由 Python launcher 取代，统管角色注册、配置与进程编排；包含只读私人日记页，通过一次性凭据和本地 IPC 分页读取、按篇解密，不直接访问 SQLite 或 Keychain。
* **个性化与界面**：动画表现 / 业务逻辑分离；Dashboard 风格设置界面；Light / Dark 主题整窗平滑过渡；自绘控件（开关、圆形滑块、动画下拉框、胶囊滚动条）；部分配置热加载。
* **可选语音合成**：Python + GENIE / `genie-tts`（GPT-SoVITS 轻量推理），默认关闭，支持中 / 日 / 英预设与自定义角色。

---

## 🚧 未完成 / 开发中

* **真实 Embedding 生产接入**：`OnnxEmbeddingProvider`、WordPiece tokenizer、`SqliteEmbeddingIndex` 与模型下载器已经实现，但桌宠生产组装和模型分发尚未完成；未注入 Provider 时检索会降级为 Noop。
* **RRF 三通道融合检索**：向量 / 关键词 / 图谱三路召回 + 排名融合（计划阶段）。
* **图谱检索融合**：关系类型图和标签共现图已落库，尚未作为完整召回通道接入 RRF。
* **成长体验完善**：当前人格成长采用可控的确定性证据门槛；情绪历史轨迹、自适应阈值、成长历史 UI 与可视化编辑暂未实现。
* **私人日记增强**：MVP 暂不提供全文搜索、导出、跨设备同步、重连配对或持久化访问审计。
* **聊天体验**：基于情绪的本地简易对话；LLM 自由对话的语气一致性与工具调用反馈打磨中。
* **物理系统 / 节日皮肤 / 多实例**：全局物理模拟、节日限定饰品、多宠物同屏等尚未实现。

---

## 📋 未来计划

* 在受支持的 Windows 环境完成启动、对话、工具、记忆恢复、睡眠整理和私人日记的端到端验证。
* 将 ONNX embedding 注入生产检索链路，随后落地向量 / 关键词 / 图谱三路 RRF 融合。
* 继续完善 `Trigger + Policy + Action` 调度策略，利用现有天气、节假日、电量、网络状态和用户空闲能力提供克制的主动陪伴。
* 记忆质量评估（retrieval_hit_rate / utility_score）与按 DesktopPet 自身指标微调遗忘参数。
* Cross-Encoder 重排序、记忆 ID 语义化、SQLite 连接池等工程优化。

---

## 🛠️ 技术栈

* **语言**：C++20（核心）、Python（launcher / 语音）
* **UI / 图形**：Qt 6 Widgets + 自绘控件、OpenGL、TinyGLTF
* **存储**：Qt Sql + SQLite（记忆主存储）
* **构建**：CMake
* **网络 / LLM**：Qt Network、OpenAI-compatible Chat Completion
* **音频 / 语音**：OpenAL / Qt Multimedia、LX Music API、可选 GENIE TTS

---

## 🚀 快速开始

**当前主要目标环境**：Windows 10 / 11、C++20 编译器（MSVC 2019+ 或 MinGW 11+）、Qt 6.x、CMake 3.20+、Python 3（launcher 与语音）。macOS 当前未支持，也未纳入运行验证。

```bash
git clone <repo> && cd Desktop-Pet
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH="path/to/your/Qt/6.x"
cmake --build .
```

启动请通过 Python launcher 拉起桌宠（负责角色注册、配置与进程编排）。模型文件与第三方运行时按需置于 `models/` 与 `third_party/`，相关拉取步骤见构建脚本。

> 配置示例见 `config/default_common_config.example.json`；API 密钥等敏感信息请放在本地配置，勿提交仓库。

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
├── tests/         # Qt Test 单元测试
├── third_party/   # 第三方库（TinyGLTF 等）
└── models/        # 推理模型文件（按需下载，不入库）
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

* `memory_improvement_plan.md`：记忆框架改进方案与实施进度
* `daydream.md`：桌宠空闲记忆整理（Daydream）设计
* `前端修改计划.md`：Python 前端改造方案
* `.evo/tasks/desktop-pet-self-evolution/桌宠自主迭代-桌面端系分.md`：自主迭代总体技术设计
* `.evo/tasks/desktop-pet-self-evolution/桌宠自主迭代-桌面端-impl-plan.md`：Task 1A–5 实现与静态验收记录
