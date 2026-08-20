# 桌宠自主迭代任务上下文

## 需求来源

- 用户与 Codex 关于 RSI、外置认知机制和 DesktopPet 自主迭代的连续讨论。
- 技术设计：`.evo/tasks/desktop-pet-self-evolution/桌宠自主迭代-桌面端系分.md`。
- 实现基线：`master` commit `1502f9f`；功能分支 `feat/agent-identity-reflection`。

## 决策记录

- [2026-08-20] 现阶段只实现角色连续性、反思与只读日记入口；Skill/Tool 自修改治理及基础模型权重/代码自修改留到稳定版本后的实验 fork。
- [2026-08-20] 情绪能力只提供 `EmotionStateProvider` 与无副作用的 `NullEmotionStateProvider`；不实现情绪算法、衰减、Appraisal 或情绪表。
- [2026-08-20] 人格变化缓慢、稳定、静默；内心活动只保存专门生成的心理摘要，不保存原始 chain-of-thought。
- [2026-08-20] 日记对模型是私人物品；所有者可经模型不可见的只读 IPC 页面访问，Python launcher 不直接读取 SQLite 或密钥。
- [2026-08-20] LLM 初始人格使用独立 `IdentityBaseline`，不复用或改变提醒系统 `PetPersonality`。
- [2026-08-20] 私有正文使用 libsodium XChaCha20-Poly1305 AEAD；每角色密钥由 QtKeychain 保存到 OS Keychain。依赖或 Keychain 不可用时禁用私有反思，不回落明文。

## 设计偏差

- [resolved] 原 §3.4.5 引用了仓库中不存在的 `PromptRenderer::buildVariables`；已改为实际存在的 `ContextManager::buildMessages` 与 `ContextBuilder::buildSystemPrompt/buildRuntimeContext`。
- [resolved] 原设计把 `PetPersonality` 当作 LLM traits/speakingStyle 基线，但实际类型只有提醒概率、时间扰动和提醒话术；已改为新增独立 `IdentityBaseline`。
- [resolved] 原设计只写“成熟 AEAD/OS Keychain”而未指定依赖；已确定 libsodium XChaCha20-Poly1305 + QtKeychain，并补充 nonce、AAD、密钥边界和失败降级。
- [resolved] 仓库不存在设计接口使用的通用 `Result<T, DomainError>`；已在 §2.3 明确由 Task 1 新增共享 `domain_result.h`。
- [resolved] `StatisticManager::recordLlmUsage` 现有签名没有 role/provider/model；已明确保留旧签名并新增维度重载。
- [resolved] SleepCycle 不能直接读取私有 `AIBrain::m_busy`，且 `AgentScheduler` 没有任务冲突查询；已明确新增只读 `isBusy` 和 `hasTaskDueBefore` 接口。

## 待回溯设计的发现

- 暂无 pending 项。
