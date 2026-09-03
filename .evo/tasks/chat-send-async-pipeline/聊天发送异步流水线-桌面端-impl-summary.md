# 聊天发送异步流水线实现总结

## 实现概述

### §3.2 异步聊天准备与模型派发

- `AIBrain::triggerThink` 的 GUI 路径仅保留请求校验、回复状态发布和不可变准备任务投递。
- persona、identity、记忆召回、关系扩展和 prompt 构建在专用 `ChatPreparationExecutor` 线程执行。
- 准备结果通过 `requestId + generation + messageId` 门禁后续接既有流式模型状态机。
- stop、restart、准备失败、迟到结果和本地路由均保持受控行为。

### §3.3 延迟副作用与性能保障

- 新增有界 FIFO `ChatSideEffectQueue`，在线程内创建并关闭独立 SQLite 连接，顺序提交 runtime event、记忆强化、用户记忆 mutation、工具结果和 AI 调用日志。
- 模型派发不再等待 SQLite 或 JSONL 持久化；session barrier 提交后才触发反思和 session 释放。
- 用户记忆和工具结果先在 GUI 线程做纯内存 staging，再以不可变 `MemoryMutationBatch` 后台持久化；投递失败会回滚内存状态。
- 队列默认容量 256，满载优先丢弃非关键日志并为 barrier 预留容量；stop 非阻塞，restart 等待旧 worker 完成线程内资源关闭。
- README 已记录当前架构、性能预算、同步工具执行边界和完整 Agent Actor 的量化迁移条件。

## 主要变更文件

- `core/ai/chat/chat_preparation_executor.*`
- `core/ai/chat/chat_preparation_types.h`
- `core/ai/chat/chat_side_effect_queue.*`
- `core/ai/ai_brain.*`
- `core/ai/ai_brain_loop.cpp`
- `core/ai/ai_brain_router.cpp`
- `core/ai/memory/memory_store.*`
- `core/ai/memory/memory_policy.*`
- `core/ai/ai_call_logger.*`
- `core/ai/runtime/agent_runtime_services.*`
- `tests/test_chat_preparation_executor.cpp`
- `tests/test_chat_side_effect_queue.cpp`
- `tests/test_streaming_dialogue.cpp`
- `tests/test_memory_strategy.cpp`
- `README.md`
- `CMakeLists.txt`

## 设计扩展

- §3.2 为准确获得 profile/runtime 路径与完整快照，增加 `chatPreparationRuntimeMetadata()` 值接口。
- §3.3 增加 `UserMemoryWrite`、`tryEnqueue*` 和 `MemoryMutationBatch`，用于在不改变外部聊天协议或 SQLite schema 的前提下移除发送关键路径中的用户记忆及工具结果写盘。
- 详细记录见 `context.md` 的“设计偏差”和审查文档。

## TDD 与验证

- Task 1 按 RED -> GREEN 实现，并通过准备执行器、流式对话、identity 和 runtime services 定向测试。
- Task 2 先以缺失队列、SQLite-only worker、缓存不一致、工具结果同步写盘及生命周期用例建立 RED，再完成 GREEN 和独立复审。
- 最终定向结果：`chat_side_effect_queue_tests` 15/15、`streaming_dialogue_tests` 29/29、`chat_preparation_executor_tests` 15/15；memory staging/强化相关 5 个指定用例全部通过。
- `git diff --check` 通过。
- 按用户要求未运行仓库全量测试；未运行 ONNX、完整桌宠、OpenGL 或 3D 资源路径。

## 后续验证

- 在 Windows 实机观察发送瞬间帧时间、UI acknowledge、准备至派发延迟和副作用队列深度。
- 若同步工具执行稳定占用一帧以上，优先迁入独立 executor；仅在 README 所列量化条件触发时评审完整 Agent Actor 迁移。
