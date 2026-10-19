# Task 2 验证记录

日期：2026-10-19。关联系分 §3.3。Task 状态、父级 CR 和提交由主 agent 维护。

## Sanity check

- `GatewayTool::parameterSchema()` 与 `PluginRuntime::executeGateway(...)` 是原注册工具和旧插件共同入口。新增 query/limit/cursor 均可选，ref/invoke 参数保持原约定。
- `PluginRuntime::discoverCapabilities(const ContextSpec&, bool) const` 保留原入口。与 queryCapabilities 共用 `discoverCapabilitiesLocked`，在同一 state 锁内获取授权目录、catalogRevision、learnedRevision，继续调用原 findGrant；过滤不绕过宿主配置。
- `setLearnedSkills(...)` 实际位于 plugin_gateway.cpp；按稳定 ID 排序后比较完整 SkillEntry.toJson，相同快照及仅顺序变化不递增 learnedRevision，真正内容变化才更新。
- `PluginCatalog::discover()` 提供稳定身份/摘要，不读取操作正文。匹配使用完整摘要，显示裁剪只在选出结果后执行。
- `AIBrain::beginActivitySelection` 为真实候选组装入口。搜索来自每条 cue.text 的前 128 字符，合并最多 1024 字符，不修改原 cues；owner 回调推进浏览位置，configureAutonomyConsumer 重置。ActivitySelector 原 16 条边界保留。
- 测试继续使用 tests/test_*.cpp + QtTest + CMake；实际音乐目标为 lx_music_plugin_tests / LxMusicPluginTests，Python 适配器组为 LxMusicAdapterTests。

## 实现与行为覆盖

| 约定 | 验证 |
| --- | --- |
| 30 个混合 Skill/Tool 稳定分页 | discoveryGateway_pagesMixedDirectoryWithoutBodies：默认 8/8/8/6，total/hasMore/nextCursor 正确，无重复/漏项，每条引用可按原路径加载，摘要不含正文/Schema |
| 中英匹配、同分稳定身份、显示有界 | discoveryGateway_unicodeQueriesRankAndBoundDisplayOnly：后排中文用途、中文自然句、mUsIc、精确 ID/name、无匹配；500 字摘要末尾仍可搜索，输出带省略号 |
| 输入、范围与旧游标 | discoveryGateway_validatesInputsAndCursorScope：limit 类型/范围/小数、query 类型/长度、cursor 类型/损坏/偏移越界、query/profile/autonomous/context 变化、禁用/重新配置 |
| learned 快照版本 | discoveryGateway_learnedSnapshotOnlyExpiresOnContentChange：相同快照/重排可续页；私有 instruction 真正改变后旧游标被拒绝 |
| 运行时重建 | discoveryGateway_recreatedRuntimeRejectsOldCursor：新实例即使数字 revision 相同，旧游标仍失效；当前实例第一页正常 |
| 相关项与探索覆盖 | capabilityCandidates_relatedTailAndExplorationCoverEveryEntry：40 条授权摘要、12 条后排相关项、1 条 disabled；前 8 条相关，其余相关与不相关项均轮转，中文自然句/英文/无匹配情况下均覆盖全部 40 条，无重复/禁用项 |
| 真实 AIBrain 集成 | autonomousSelection_rotatesAllAuthorizedSummaries：5 次选择覆盖全部 30 条已授权摘要，每次最多 16 条，1 条未授权摘要不出现，无正文哨兵 |
| 旧插件与结果语义 | PluginRuntimeTests、AutonomousChatTests、LxMusicPluginTests 全套继续验证旧格式 1、command/受控 MCP、聊天恢复、音乐 discover/load/invoke；未改旧插件清单 |

## RED → GREEN

| RED 证据 | 原失败 | GREEN |
| --- | --- | --- |
| /tmp/plugin-context-task2-red-test.log | 4 个网关测试失败：缺 total/nextCursor，query/limit 被旧 Schema 拒绝 | 新网关测试全部通过 |
| /tmp/plugin-context-task2-autonomy-red-test.log | 5 次真实自主选择仍只有前 16 条 | 实际覆盖 30 条授权摘要 |
| /tmp/plugin-context-task2-review-red-test.log | 中文自然句结果为空；新实例接受旧 cursor | 汉字二元片段匹配、实例 nonce 后全部通过 |
| /tmp/plugin-context-task2-candidates-red-test.log | 中文自然句未保留后排相关项 | 中英文及有/无相关项的探索覆盖全部通过 |

父级 CR 的中文句子匹配、运行时实例 nonce、cue 查询上限均已回填系分；不新增持久化版本、联网检索或模型调用。无新增依赖、用户配置修改、真实模型或真实外部服务调用。

## 验证命令与结果

```sh
/Users/huang/.qt-build-env/bin/cmake --build build --target plugin_runtime_tests activity_selector_tests autonomous_chat_tests lx_music_plugin_tests -j 4
QT_QPA_PLATFORM=offscreen /Users/huang/.qt-build-env/bin/ctest --test-dir build --output-on-failure -R '^(PluginRuntimeTests|ActivitySelectorTests|AutonomousChatTests|LxMusicPluginTests|LxMusicAdapterTests)$'
```

五组 CTest 全部通过，38.98 秒，日志 `/tmp/plugin-context-task2-final-ctest.log`，完整 QtTest 输出另存 `/tmp/plugin-context-task2-final-details.log`。计数：PluginRuntime 29、ActivitySelector 22、AutonomousChat 6、LxMusicPlugin 8 passed / 1 skipped；Python LxMusicAdapter 24/24。音乐跳过项为原有真实播放器 opt-in 验收，本次对应环境变量为空。

首轮 Python adapter 的 test_launch_and_scheme_argv 在 2 秒内未收到假播放器 argv 文件；该用例原样独立复跑通过（1.13 秒，`/tmp/plugin-context-task2-adapter-retry.log`），完整 24 项复跑也通过。未因单次环境时序失败改动适配器或扩大 Task 范围。

最终清理 Qt fromUcs4 弃用重载并收紧 cue 查询长度后，四目标构建无新增 warning/error，日志 `/tmp/plugin-context-task2-final-build.log`；以相同 CTest 命令去掉 LxMusicAdapterTests 后，四组 C++ 复验 **4/4 通过，23.94 秒**，日志 `/tmp/plugin-context-task2-final-cpp-ctest.log`。git diff --check 通过。

公共文档已补查询/分页、重启游标失效、完整定义与请求投影、8 轮聊天/独立 Task 预算和候选轮转。文档里的 96/96 契约复验由父级独立执行确认，证据 `/tmp/plugin-context-wave-contract.json`；本 Task 未重复安装依赖或运行该脚本。
