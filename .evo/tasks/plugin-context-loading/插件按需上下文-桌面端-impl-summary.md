# 插件按需上下文实现总结

日期：2026-10-19。实现对应[系分](./插件按需上下文-桌面端-系分.md) §3.2、§3.3，进度以[实施计划](./插件按需上下文-桌面端-impl-plan.md)为准。

## 已实现的行为

- §3.2：对话与长任务共用请求投影。当前请求保留最近一个 Tool 的两个操作和最近一个 Skill，公共说明去重，回收的操作须重新加载。Schema、effects 和 result 完整传递，元数据超过 32 KiB UTF-8 JSON 时整项报错。
- §3.2：discover/load 不再写入生活或工作记忆，完整脱敏结果保存在独立日志和原任务步骤中。模型缺操作定义时不派发；内部既定回复与原权限校验继续使用原路径。普通聊天最多 8 轮工具，容纳正常顺序加载。
- §3.3：摘要支持中英文关键词和中文句子匹配，默认每页 8 条、最多 16 条，游标绑定实例、版本、查询及范围。相同 learned 快照不会无故使游标失效。
- §3.3：自主候选保留最多 8 条相关能力，再轮转补充剩余条目，避免只看固定前 16 条。搜索线索有界，实际身份、记忆和执行授权不变。

## 主要文件

| 文件 | 用途 |
| --- | --- |
| core/ai/plugin/capability_context.* | 请求副本投影与当前操作定义检查 |
| include/ai_types.h、core/ai/tools/runtime/tool_result_sanitizer.cpp | 宿主元数据标识、完整定义预算 |
| core/ai/ai_brain_loop.cpp、ai_brain_router.cpp、task/task_orchestrator.* | 对话、记忆和长任务接入 |
| core/ai/plugin/capability_discovery.*、plugin_runtime.*、plugin_gateway.cpp | 文本搜索、分页和原网关接入 |
| core/ai/ai_brain_autonomy.cpp、ai_brain.h | 自主候选轮转及聊天轮次上限 |
| tests/test_capability_context.cpp 及原有五个相关 QtTest 文件 | 投影、聊天、Task、分页与真实自主选择测试 |
| contracts/tools/插件运行时接入.md、README.md、插件系统开发指导.md | 作者接入与运行时公开约定 |

## TDD、审查与实测

两个 Task 均先复现缺陷再实现到通过，且由父级完成 CR。每项行为与具体测试、RED/GREEN 日志的对应关系见 [Task1-验证记录](./Task1-验证记录.md)、[Task2-验证记录](./Task2-验证记录.md)，审查结论见[审查记录](./插件按需上下文-桌面端-审查.md)。

同一份合成的 12 次加载历史中，对话消息数组由 36,843 降为 8,455 UTF-8 字节（约 77.1%），Task steps 由 35,235 降为 7,114 字节（约 79.8%），公共说明 12→1 份。这个测试测量请求材料体积，没有调用真实模型测量 token 或费用。

## Wave 最终验证

2026-10-19 在 macOS、Qt 6.8.3、现有 build/ 与 launcher/.venv 环境下完成：

```sh
/Users/huang/.qt-build-env/bin/cmake --build build -j 4
QT_QPA_PLATFORM=offscreen /Users/huang/.qt-build-env/bin/ctest --test-dir build --output-on-failure
PYTHONPATH=/private/tmp/desktop-pet-contract-validation.97hLVY launcher/.venv/bin/python contracts/tools/verify_contract.py
git diff --check
```

- 全项目构建成功，包含 Desktop_Pet；仅有未修改文件中的原有弃用警告。日志：`/tmp/plugin-context-wave-build.log`。
- 全量 CTest **66/66 组通过，146.77 秒**，覆盖启动器聊天、图片、记忆、任务、自主活动、插件运行时和音乐适配器。日志：`/tmp/plugin-context-wave-ctest.log`。项目原有真实模型和真实播放器 opt-in 用例未启用。
- 独立契约校验 **96/96 通过**，退出码 0、stderr 为空。复用已有临时校验依赖，未安装新依赖。结果：`/tmp/plugin-context-wave-contract.json`。
- `git diff --check` 通过。两个 Task 的实现、审查、设计回填和 Wave 验证均已完成。

## 设计细化与兼容范围

实施中补足 Task 门禁前置、普通聊天 3→8 轮、完整元数据独立归档、中文自然句片段匹配、游标实例标识和自主查询文本上限，均已回填系分，原因见 [context](./context.md)。没有遗留待回溯项。

Plugin/Skill/Tool 格式 1、旧音乐包、command/MCP、授权及回执契约继续兼容。没有数据库迁移、新增依赖、用户配置修改或真实外部服务调用。真实模型行为与真实播放器仍使用项目原有 opt-in 验收，不由受控测试代替。
