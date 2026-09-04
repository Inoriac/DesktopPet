# 表现层重构调研：模型格式 / 渲染引擎 / 渲染风格

> 状态：调研完成，待决策。
> 调研日期：2026-09-05。
> 调研方式：代码现状盘点 + GitHub 生态检索（star / license / 活跃度均为当日实测）。

## 1. 结论

**推荐主路线：VRM 1.0 作为目标模型格式，渲染引擎分两步走。**

1. **格式**：VRM 1.0。它是 glTF 2.0 的扩展，现有 tinygltf 管线可以直接加载其几何体；它提供标准化人形骨骼（humanoid）、标准化表情（expressions）、弹簧骨骼物理（spring bone）和 MToon 卡通材质规范——这四项恰好都是当前自研管线缺失、且对"Agent 驱动的桌宠"价值最高的能力。模型生态（VRoid Studio / VRoid Hub / Booth）远大于其他格式。
2. **引擎**：短期**保留自研 GL 引擎**并叠加 toon/MToon 着色器与 VRM 扩展解析（增量路径，风险最低）；中期用 **threepp**（C++20、MIT、three.js 风格 API、GL3.3+Vulkan 双后端）做一个并行原型，验证是否值得整体迁移。
3. **C++ 生态的现实**：**不存在成熟的 C++ VRM 运行时**（Unity 有 UniVRM、JS 有 three-vrm、UE 有 VRM4U，C++ 是空白）。这意味着 VRM 支持必须自建，但 VRM 本质是"glTF + 几个 JSON 扩展"，自建成本可控，且做出来就是本项目的差异化资产。

**不推荐**：PMX/MMD（生态库已停滞、模型授权风险高）、Live2D（2D 风格转向，属另一个产品）、Filament/OGRE/Unity/Godot（过重或方向不符）。

---

## 2. 现状盘点（代码实测）

| 组件 | 现状 |
|---|---|
| 视口 | `RenderViewport : QOpenGLWidget`，GL 3.3 Core |
| 引擎 | 自研 `RenderEngine`，engine/ 目录共约 2600 行 |
| 模型加载 | tinygltf（几何 / 骨骼 / 动画三件套：`model_loader*.cpp` + `animation_importer.cpp`） |
| 材质 | 自定义 PBR uniform，无 toon/NPR 着色器（grep 无 toon/outline/cel 命中） |
| 交互 | 自研碰撞体 + 射线拾取（`checkHit`）、Y 轴转向 + 头部补偿的鼠标追踪 |
| 透明窗口 | 聊天气泡已用 `WA_TranslucentBackground` 模式，主窗口同路线 |
| VRM/MToon | 无任何支持 |

**关键约束**（任何替换方案必须满足）：

1. 透明背景渲染（桌宠窗口无背景）。
2. 身体部位级命中检测（触摸反馈依赖自研碰撞体）。
3. 与 Qt 事件循环 / GUI 线程预算共存（聊天链路 P95 < 4ms，渲染不得侵占）。
4. 轻量：常驻内存与 GPU 占用必须小（桌宠是常驻进程）。
5. Windows 10/11 主目标，C++20，CMake。

---

## 3. 模型格式候选

### 3.1 VRM 1.0（推荐）

VRM = glTF 2.0 + VRMC_* 扩展。对本项目的价值点：

| VRM 能力 | 对本项目的意义 |
|---|---|
| **标准化 humanoid 骨骼** | 任意 VRM 模型骨骼命名一致 → 动画天然跨模型复用；鼠标追踪可映射到标准 `lookAt`，替代现有自研头部补偿逻辑 |
| **标准化表情（expressions）** | `happy/angry/sad/relaxed/surprised/neutral` + 口型/眼神 blendShape → **与情绪系统的六情绪标签几乎一一对应**，Agent 可直接用工具按名触发表情 |
| **Spring Bone（弹簧骨骼）** | 头发/衣物/饰物自主晃动，"活物感"的核心来源，纯本地物理，零 LLM 成本 |
| **MToon 材质规范** | 卡通渲染的工业标准（lit/unlit 混合、边缘光、描边宽度、cutoff），规范公开可直接实现 GLSL 版 |
| **模型生态** | VRoid Studio 免费建模、VRoid Hub / Booth 海量模型，用户可自带角色（对"角色身份/profileId 隔离"架构是内容放大器） |

**C++ 库现状（实测）**：

| 库 | 定位 | Stars | License | 最近活跃 | 评价 |
|---|---|---|---|---|---|
| `infosia/VRM.h` | header-only VRM 1.0 JSON 序列化/反序列化 | 21 | MIT | 2026-07 | 小而新，可 vendor 或作参考；只解决"解析"，不解决"运行时" |
| （无成熟运行时） | — | — | — | — | UniVRM=C#、three-vrm=JS、VRM4U=UE、godot-vrm=GDScript，C++ 空白 |

**结论**：VRM 支持需自建 = tinygltf（已有）+ VRMC 扩展解析（humanoid 映射 / expressions / spring bone / MToon 参数）+ 对应运行时。VRM 1.0 规范是公开的 JSON schema，解析层工作量约等于再写一个 `animation_importer` 量级的模块；运行时（表情混合、弹簧骨骼）是主要工作量。

注意：市面存量模型大量是 **VRM 0.x**（扩展名为 `VRM` 而非 `VRMC_*`，表情叫 blendShapeClip）。建议运行时以 1.0 为主、0.x 做兼容层（两者差异集中在扩展命名与表情/lookAt 结构，几何与骨骼一致）。

### 3.2 PMX / MMD（不推荐）

| 库 | 定位 | Stars | License | 最近活跃 |
|---|---|---|---|---|
| `benikabocha/saba` | 完整 MMD 运行时（PMD/PMX/VMD + toon + MMD 物理） | 507 | MIT | **2023-09（停滞）** |
| `oguna/MMDFormats` | PMD/PMX/VMD 解析器 | 165 | CC0 | **2022-02（停滞）** |

- 动漫风格成熟，VMD 动画文件多；但核心库均已停滞 2-3 年。
- **模型授权是硬伤**：PMX 模型普遍附带"禁止再配布/禁止改造"类社区条款，作为可分发产品的默认内容风险高。
- 无标准化表情语义（表情是模型自定义命名），Agent 按名驱动表情需要逐模型适配——与本项目"Agent 驱动表现"的方向相悖。

### 3.3 保持纯 glTF（现状）

零迁移成本，但没有 humanoid/表情/弹簧骨骼标准，每个模型都要手工配置映射。适合作为 VRM 之下的 fallback 通道，不适合作为演进方向。

### 3.4 Live2D（风格转向，另议）

Cubism Native SDK（C++）持续维护（官方 samples 2026-04 仍有更新），个人/小团队免费授权（有收入门槛）。但这是**从 3D 转向 2D** 的产品决策，不是"更好的 3D 表现层"，且与现有 GLTF 资产、骨骼交互、鼠标追踪全部不兼容。仅当产品方向改为 2D 桌宠时考虑。

---

## 4. 渲染引擎候选

### 4.1 方案对比（实测数据）

| 方案 | Stars | License | 活跃 | 形态 | 与本项目契合度 |
|---|---|---|---|---|---|
| **保留自研 GL 引擎** | — | — | — | 自有 2600 行 | ★★★★★ 零迁移成本，碰撞体/追踪/Qt 集成全部保留 |
| **threepp** | 913 | MIT | 2026-09（活跃） | C++20 完整 3D 库（three.js API） | ★★★★☆ 能力最全，迁移成本中高 |
| **bgfx** | 17456 | BSD-2 | 2026-09（活跃） | 底层跨后端渲染器 | ★★★☆☆ 只解决渲染提交，场景/动画仍需自建 |
| **Magnum** | 5204 | MIT 系 | 2026-08（活跃） | 中型引擎 + 插件体系 | ★★★☆☆ glTF 导入成熟，但 API 面大 |
| **sokol_gfx** | — | MIT | 活跃 | 单头文件极轻量 | ★★☆☆☆ 太底层，等于重写引擎 |
| OGRE / Filament / Diligent / The Forge | — | — | — | 重型引擎 | ★☆☆☆☆ 过重或 PBR 导向，与桌宠需求错位 |

### 4.2 threepp 详评（唯一值得做迁移原型的候选）

实测其仓库结构，关键能力全部具备：

- **C++20**（与本项目标准一致）、MIT、Conan Center 有包。
- **双后端**：OpenGL 3.3 raster（可移植基线，含 Emscripten/WebGL2 目标）+ Vulkan 延迟渲染器（带光追 AO/GI/反射，未来可选）。
- **完整动画栈**（three.js 移植）：`AnimationMixer / AnimationClip / AnimationAction / SkinnedMesh / Skeleton / MorphTargets`——**morph targets 正是 VRM 表情的载体**。
- **加载器**：GLTFLoader / FBXLoader / OBJ / Collada / URDF 等。
- **骨骼网格射线拾取**：存在 `SkinnedMeshRaycast_test.cpp`，说明命中检测可在 threepp 上重建（替代现有自研碰撞体需要验证精度与性能）。
- 场景图 + 材质 + 光照 + 相机 + 控制器开箱即用。

**风险**：
- 替换 2600 行已调通的自研代码（碰撞体、追踪、骨骼快照混合）是一次大手术。
- Qt 集成需自行处理：threepp 渲染到 FBO → Qt 合成，或用 `QOpenGLWidget` 共享 GL context；透明背景混合需验证。
- 913 stars 的单人主导项目，bus factor 需要评估（好在 MIT，最坏情况可 fork 自维护）。
- VRM 层仍需自建（threepp 无内置），但 three.js 生态有 three-vrm 可作逐行参考。

### 4.3 bgfx 定位说明

bgfx 解决的是"一份代码提交到 GL/Vulkan/D3D/Metal"，**不提供**场景图、动画、加载器。选它意味着在现有自研引擎之下换渲染提交层——对桌宠这种单角色小窗口场景，GL 3.3 远未触及瓶颈，收益不匹配成本。仅当未来明确需要 Vulkan/D3D（如 Windows 上 GL 驱动兼容性问题）时再评估。

### 4.4 "外接渲染器"选项（进程外表现层）

用户设想中"外接其他用于渲染表现层的东西"，技术上有两条路：

1. **QWebEngine / WebView2 + three.js + three-vrm**：JS 侧 VRM 运行时最成熟（three-vrm 是 pixiv 官方维护），代价是 Chromium 常驻 100-200MB 内存 + C++↔JS IPC，且命中检测/触摸反馈要跨进程重建。**对"轻量"目标是负资产，不推荐作为主渲染层**；但作为 Dashboard/设置页的预览窗口倒是可选。
2. **独立渲染子进程 + 共享纹理/窗口**：复杂度高出一个量级，无对应收益，不推荐。

---

## 5. 风格渲染（toon / NPR）

无论引擎选谁，风格层都是独立工作量：

- **MToon 1.0**（VRM 官方材质规范，公开）：lit/unlit 混合系数、参数化边缘光（rim）、描边宽度、阴影色、cutoff 透明。GLSL 实现参考充分（Unity 原版、babylon-mtoon-material 76★、godot-vrm 461★ 均可对照移植）。
- **描边**：inverted hull（背面外扩）两 pass 方案，任意引擎均可实现，成本极低。
- **与现有 PBR 共存**：建议材质系统按 `PBR / MToon / Unlit` 三分支路由，VRM 模型走 MToon，存量 glTF 走 PBR，不做二选一。

---

## 6. 物理（spring bone）

VRM spring bone 是带阻尼 的 Verlet 弹簧链 + 球/胶囊碰撞体，**不需要 Bullet/PhysX**：

- 自研实现约 200-400 行（各 VRM 运行时均为自研，无通用 C++ 库可用）。
- 计算量：每帧每骨骼链一次 Verlet 积分，单角色通常 < 100 个弹簧节点，CPU 成本可忽略。
- 现有 `checkHit` 已有胶囊/球碰撞几何代码，可复用。
- PMX 的刚体物理（Bullet）才是重量级方案——这也是不选 PMX 的理由之一。

---

## 7. 与 AI 系统的协同（本项目特有收益）

表现层升级对 Agent 框架的直接放大效应：

1. **情绪 → 表情直连**：情绪系统六标签（Neutral/Joy/Sadness/Anger/Fear/Surprise）与 VRM expressions（neutral/happy/sad/angry/surprised/relaxed）几乎一一对应，`EmotionSystem` 状态变化可直接投影为表情权重，无需 LLM 参与。
2. **新增 Agent 工具**：`set_expression`（按名触发表情+强度）、`set_look_at`（视线目标）可注册进 ToolRegistry，走现有 L0-L4 策略（均为低风险 L0/L1）。
3. **自主活物感**：spring bone + 呼吸/idle 微动画是纯本地物理，让角色在"无对话时也在活着"，符合 `Trigger + Policy + Action` 主动陪伴规划，零 token 成本。
4. **用户自带角色**：VRM 生态让用户可以用 VRoid 自建角色导入，与 profileId 角色隔离架构天然契合。

---

## 8. 推荐路径（分阶段）

**Phase 0 — 风格升级（不动格式，1-2 周量级）**
- 现有引擎加 MToon/toon 着色器 + inverted hull 描边；材质系统加 MToon 分支。
- 验收：存量模型可切换 toon 风格渲染，帧率无回归。

**Phase 1 — VRM 接入（自研管线，核心阶段）**
- VRMC 扩展解析（可 vendor `infosia/VRM.h` 或自写）：humanoid 映射、expressions、spring bone、MToon 参数；兼容 VRM 0.x。
- 运行时：morph target 表情混合、spring bone Verlet 求解、MToon 材质实例化。
- 新增 `set_expression` / `set_look_at` 工具；情绪系统投影表情。
- 验收：加载 3+ 个来源不同的 VRM 模型，表情/物理/命中检测全部工作。

**Phase 2 — threepp 迁移评估（可选，并行原型）**
- 独立分支做 threepp 原型：加载 VRM → 渲染进 Qt 透明窗口 → 骨骼拾取 → 性能对比。
- 量化门槛（任一不达标即放弃迁移，继续自研引擎）：
  - 透明背景混合正确且无闪烁；
  - 常驻内存增量 < 50MB；
  - 骨骼拾取精度不低于现有碰撞体方案；
  - GUI 线程预算不受影响。

**Phase 3 — 内容管线**
- VRoid 建模 → 导出 → 本地导入的文档化流程；模型授权白名单策略（沿用现有安全运行时的白名单思路）。

---

## 9. 风险与待验证

| 风险 | 缓解 |
|---|---|
| C++ VRM 运行时全部自研，工作量集中在 Phase 1 | 规范公开 + three-vrm/UniVRM 可逐行参考；先做 1.0 最小集（humanoid+expressions），spring bone/MToon 随后 |
| VRM 0.x 存量模型兼容 | 几何骨骼一致，仅扩展层差异，做适配层而非双运行时 |
| threepp bus factor（单人主导） | MIT 可 fork；且 Phase 2 是可选评估，不构成依赖 |
| 模型授权（用户自带内容） | 导入时记录来源元数据；默认内容只用明确可再配布模型（VRoid 官方示例等） |
| Qt 透明窗口 + 第三方引擎混合 | Phase 2 第一周先做 FBO→Qt 透明合成 spike，不通则提前止损 |

## 10. 附：候选库速查表

| 库 | 用途 | License | Stars | 状态 |
|---|---|---|---|---|
| markaren/threepp | 完整 3D 库（three.js API，GL/Vulkan） | MIT | 913 | 活跃（2026-09） |
| bkaradzic/bgfx | 跨后端渲染提交层 | BSD-2 | 17456 | 活跃（2026-09） |
| mosra/magnum | 中型引擎 + 插件 | MIT 系 | 5204 | 活跃（2026-08） |
| infosia/VRM.h | VRM 1.0 JSON 解析（header-only） | MIT | 21 | 活跃（2026-07） |
| benikabocha/saba | MMD 完整运行时 | MIT | 507 | 停滞（2023-09） |
| oguna/MMDFormats | PMD/PMX/VMD 解析 | CC0 | 165 | 停滞（2022-02） |
| Live2D Cubism Native | 2D 骨骼动画 SDK | 专有免费（收入门槛） | — | 活跃（2026-04） |
| （参考）pixiv/three-vrm | JS VRM 运行时，自研时的实现参考 | MIT | — | 活跃 |
| （参考）virtual-cast/babylon-mtoon-material | MToon 移植参考 | — | 76 | — |
