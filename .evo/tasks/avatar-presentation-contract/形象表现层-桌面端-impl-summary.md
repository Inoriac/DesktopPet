# 形象表现层解耦实现总结

日期：2026-10-12。状态：本轮实现与相关自动化回归完成，全项目编译通过；全量测试 45/47，独立失败和真实窗口验收边界如下。根据用户要求，按设计、实现与测试、验收文档三个阶段分批提交本地 Git；未推送远端。

关联：[初步契约](形象表现层-桌面端-初步契约.md)、[系分](形象表现层-桌面端-系分.md)、[实现计划](形象表现层-桌面端-impl-plan.md)、[审查](形象表现层-桌面端-审查.md)、[配置说明](../../../config/AVATAR_PRESENTATION.md)。

## 1. 已交付的调用边界

主体现在通过 `AvatarPresentation` 发现形象能力、查询状态和可选目标、提交或取消请求、处理交互、控制显隐及移动。状态机在这一层统一执行。AI、情绪和交互模块不再直接使用具体动画播放器或渲染引擎。

| 位置 | 职责 |
| --- | --- |
| `core/presentation/avatar_presentation.*` | 仅依赖 QtCore 的配置与执行契约、优先级队列、请求结果、保护路径、显隐和代次管理 |
| `core/animation/avatar_3d_backend.*` | 将现有 AnimationPlayer 接入后端执行接口，报告真实播放、轮次和失败 |
| `ui/avatar_view.h` | 形象命中区域和可选相机输入的中性视图接口 |
| `engine/render_viewport.*` | 现有 3D 资源发现、绘制／计时门控及桌面移动适配 |
| `core/ai/tools/animation_tools.h` | 动态状态工具、能力查询、请求状态、取消及移动；保留旧工具参数兼容 |
| `core/ai/runtime/runtime_ui_bridge.*` | 主体通过中性形象接口访问表现能力，通用输出继续独立 |
| `core/behavior/behavior_manager.*` 与 `ui/petwindow_*.cpp` | 情绪／触摸／拖动／吸附等调用统一入口 |

未来接入 2D 时实现 `AvatarBackend`，提供相应 `AvatarView`，并在界面装配处替换具体视图。主体和模型工具无需按新动作名称改写。当前仍使用现有 3D 后端，没有新增生产用 2D 渲染器。

## 2. 规则落地

- 状态名、说明、候选动画、语义映射和打断规则由形象 JSON 提供。默认配置保留原有 22 个状态及已有转换。模型读取可选状态，实际候选由本地选择。
- 支持立即切换、当前轮次后切换、完整退出路径。使用者可覆盖某个候选动作的规则；禁用状态不能作为初态或间接过渡。
- 用户通常优先，但高强度 anger 可拒绝动画回应；阈值可配置。拒绝不丢失触摸事实，拖动位置不受身体动画保护阻塞。
- 普通取消和到期不会截断已经进入的有限保护路径。旧目标被放弃，路径收尾后复位；故障看门狗处理后端未报告边界的情况。
- 隐藏立即取消未完成动画请求、清空队列、复位配置初态，关闭帧定时器、动画推进与绘制。隐藏期间拒绝新转换；显示从初态和零进度开始，旧回调和旧队列不会恢复。
- 显隐不停止人格、记忆、情绪或任务服务。形象说明进入当前模型上下文，不改人格／记忆身份。运行时不支持更换形象。
- 支持显式 `fallbackState`，结果标识原目标、实际目标和替代关系。能力不足可由主体输出“（行为描述）”，动画失败仍保留为失败。
- `completed` 表示目标候选已实际启动；动作轮次完成是独立事件。请求具有 ID、期限及唯一终态，接受或排队不等于完成。

配置暂采用文件编辑、重启生效，完整字段和示例见[配置说明](../../../config/AVATAR_PRESENTATION.md)。气泡和语音继续沿用通用服务，本轮不增加音效。

## 3. 验证证据

环境：macOS 15.3 / arm64，Qt 6.8.3，Debug，ONNX Runtime 关闭。以下命令在仓库根目录执行：

```sh
/Users/huang/.qt-build-env/lib/python3.9/site-packages/cmake/data/bin/cmake --build build --parallel 4
QT_QPA_PLATFORM=offscreen /Users/huang/.qt-build-env/lib/python3.9/site-packages/cmake/data/bin/ctest --test-dir build --output-on-failure
git diff --check
```

| 检查 | 结果 |
| --- | --- |
| 全项目构建，含 Desktop_Pet 和全部测试目标 | 通过 |
| 本轮相关 10 个 CTest 套件 | 全部通过：Statistic、AgentRuntimeServices、IdentityState、ModuleConnectivity、StreamingDialogue、ChatPreparationExecutor、Tool、EmotionRuntime、TaskOrchestrator、AvatarPresentation |
| AvatarPresentationTests | 20 项 QtTest 结果通过，含 18 个行为测试及初始化／清理 |
| StatisticTests | 8 项 QtTest 结果通过，使用确定性骨骼／关键帧及真实 AnimationPlayer → Avatar3DBackend |
| 全部 CTest | 45/47 通过，65.70 秒；失败见下一节 |
| 静态依赖与差异检查 | 中性库仅链接 QtCore；AI／情绪／交互无具体播放器依赖；`git diff --check` 通过 |

新增契约测试经历 RED→GREEN。末轮补充的禁用初态、禁用过渡路径和通用自动转换三个测试在修复前均失败，修复后随全量运行通过。覆盖不同状态名、真实失败、循环边界、完整保护路径、拖动、隐藏／显示、取消／过期、迟到事件、同步事件重入、跨线程调用、资源缺失、替代状态及旧工具别名。

构建与测试日志保存在本机 `/tmp/avatar-final-full-build2.log`、`/tmp/avatar-final-full-tests.log`；末轮 RED 记录为 `/tmp/avatar-boundaries-red.log`。

## 4. 全量测试中的独立失败

1. **PromptRendererTests**：`testContextBuilderFallbackUsesIdentity` 仍断言旧三维人格完整句子。既有提交 `83c4b62` 扩展为七维人格，输出包含新增描述，故精确句子不匹配。相关投影、默认人格、ContextBuilder 和测试文件本轮均未修改，单测可独立复现。早期未重建的旧测试二进制通过不能证明当前源码基线通过。
2. **FileWebToolTests**：`test_execute_whitelisted_command_success` 自调用测试程序的子进程返回 9，单独重跑仍失败；直接执行其子测试通过。该测试目标和其源码本轮均未修改，也不链接形象模块。子进程为何退出尚未确定，不能将它记录为已通过或已修复。

这两项已作为独立后续事项记录。本轮未继续开展此前暂缓的人格演化功能补充，也未修改命令执行策略来绕过失败。

## 5. 仍需实际环境验收

- 仓库缺少实际角色模型，尚未验证真实角色的视觉效果、过渡姿态、命中区域及动画质量；已有动画资源不能代替模型验收。
- 尚未进行真实桌面窗口的托盘显隐和任务同时运行的端到端验收；当前证据来自契约／播放器测试、主体回归及显隐接线审查。
- Windows 原生拖动、边缘吸附、透明窗口及托盘行为未在本机执行。
- 正式选定 2D／3D 方案及具体资产后，还需实现对应生产适配器并验收资源与交互映射；可视化配置编辑器和规则热更新未纳入本轮。

## 6. 分批提交记录

1. `d1cead6` — `docs(avatar): 明确形象渲染契约与实施方案`：产品规则、技术设计、实施计划及调研关联。
2. `cfc6f48` — `feat(avatar): 解耦形象渲染并统一状态机调用`：完整实现、默认配置及配套测试，提交正文记录验证结果和待收尾事项。
3. `docs(avatar): 补齐配置指南与验收记录`：本总结、配置说明、审查结论及上下文。

以上按用户要求提交到本地分支，未执行 push。分批期间未修改已验证的实现和测试代码，因此沿用第 3 节的构建与回归结果。
