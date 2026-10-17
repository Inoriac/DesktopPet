# LX Music 通用能力接入审查

## Task 1 / §3.2

父级逐项阅读 CLI/config/输入验证、HTTP 请求/应答/超时、SQLite 查询、启动与验证路径、三份清单和 Skill；测试映射见 /tmp/lx-task1-report.md。

- 发现并修复：文本 LRC 返回兼容；截断响应不能仅凭可解析 JSON 成功；JSON false 应答不能让 skip 成功；子进程 cwd 固定程序目录；来源 ID 结构斜杠保持旧脚本语义。
- 查询无隐式启动，SQL 只读且参数化，写派发最多一次；不确定结果保留 unknown；无软件名称核心分支、新依赖、身份/记忆库修改。
- 清单与 12 个真实操作、Skill 的精确依赖一致。原生工具和旧 Skill 未替换，不会因插件未配置失去原入口。
- 全量构建通过；58/58 CTest 通过（105.50s，/tmp/lx-task1-full-ctest.log）。来源 ID 兼容修复后的专用回归单独补验，最终结果记录在实施计划。
- 原有契约 94/94 通过（/tmp/lx-contract-final.log）；新包三份 manifest 使用现存离线 jsonschema 校验，运行时 catalog 验证属于 Task 2。
- Windows 实际播放器没有在本机验证。测试中的启动器和 HTTP 后端均受控，不能等同真实软件可用。

## Wave 检查约定

各 Task 串行独占源码与构建；父级只读审查后收回所有权。新增测试沿用仓库 tests/ 与 tests/fixtures/。每个计划行为均映射到测试，锚点留痕在 context.md；无未解决的跨切片设计偏差。
