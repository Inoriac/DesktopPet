# 形象表现层解耦上下文

## 需求来源

- `形象表现层-桌面端-初步契约.md`
- `形象表现层-桌面端-系分.md`
- 用户已明确要求按规则优化系统、进行渲染层解耦。

## 决策记录

- 规则沿用 JSON 配置并于启动读取，暂不做规则编辑器或热更新。保留当前 3D 后端，用可替换测试后端检验中性契约。
- 隐藏是立即停止/重置的生命周期边界，不等待受保护动画；显示从配置初态开始。
- 整个解耦是一个垂直 Task，依据用户已确认的规则和实现指令执行，所有代码最终统一审查。
- [sanity check] 已实测 AnimationPlayer::changeState 返回 void；AnimationPlayer::update 内存在自动转换；AnimationImporter 现有解析没有打断规则；RenderViewport 用局部 QTimer 驱动绘制，paintGL 使用静态计时器。
- [sanity check] 构建目录 `build` 为 macOS Qt 6.8.3 Debug，ONNX Runtime 关闭；使用现有 CMake 配置。
- [基线验证] 实现前执行 10 个相关 CTest 套件，9 个通过：AgentRuntimeServices、AgentScheduler、ScheduleConnectivity、ModuleConnectivity、ChatPreparationExecutor、Tool、EmotionRuntime、PromptRenderer、TaskOrchestrator。StatisticTests 因引用仓库不存在的 `assets/models/milltina/model/milltina.gltf` 在 initTestCase 失败，播放器验证改用确定性骨骼/动画夹具。
- [补充锚点] `AIBrain::isToolCallAllowed` 存在 Touch 前缀约束，ConfigManager 含旧状态默认白名单，ChatPreparationExecutor 传入固定 Idle；这些也纳入同一 Task 的配置与实际状态接入。
- [交付] 已实现中性 AvatarPresentation、AvatarBackend、AvatarView 与现有 3D 接入；工具／情绪／交互统一调用入口，配置说明位于 `config/AVATAR_PRESENTATION.md`。
- [保护边界] 普通取消与 TTL 不跳过已进入的整条有限路径；隐藏和实际后端故障可立即终止。禁用中间状态在进入路径前拒绝。
- [自动转换] 非循环完成后选择具体来源的自动边，否则选择未被覆盖的通用自动边；同目标具体边覆盖通用边。
- [最终验证] 全项目构建通过，相关 10 套件通过；全部 CTest 45/47。新契约测试 20 项及实际播放器测试 8 项通过；详细命令、两项独立失败及平台边界见实现总结。
- [基线修正] 前述实现前测试使用已有构建产物。全量重建后 PromptRendererTests 暴露既有 `83c4b62` 将人格扩为七维而旧断言仍要求三维完整句子的问题；不能将早先的旧二进制通过当成当前源码基线通过。
- [分批提交] 用户要求按开发流程 commit。设计契约提交为 `d1cead6`，实现与测试提交为 `cfc6f48`，使用说明、审查和验收结果随后独立提交；未执行 push。

## 设计偏差

未新增正式 2D 渲染器、配置编辑器或规则热更新；按系分约定使用可替换测试后端和启动配置。无其他设计偏差。

## 待回溯设计的发现

具体行为与验证边界已回写系分、配置说明及实现总结，无未决的本轮实现设计问题。
