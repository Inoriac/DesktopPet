# 插件系统指导与准入契约

本目录面向给予模型使用软件或运用技能的能力。Plugin 是安装、分发和管理单位，可以包含提供方法的 Skill、提供可执行操作的 Tool，或两者都有；模型根据各条目的简短描述选择并按需加载。

开发请先阅读 [插件系统开发指导](./插件系统开发指导.md)，实现接入见[插件运行时接入](./插件运行时接入.md)。主体与插件的后续实施以[自主 Agent 设计](../agent/自主Agent-身份记忆与活动运行时-桌面端系分.md)为主入口，按人物与记忆归属、活动运行机制、外部实践的依赖顺序推进。Tool 声明采用 JSON 清单＋Markdown 使用说明，支持脚本、CLI 和 MCP。Plugin、Skill 与 Tool 分别使用对应 Schema；通用运行时负责摘要发现、按需加载、宿主授权与异步执行。

摘要发现支持中英文查询和稳定分页，默认每页 8 条，可用 nextCursor 继续浏览。自主候选按活动线索筛选并轮转探索；选中后才加载完整说明和操作 Schema。模型请求会去重公共说明并回收旧能力材料，完整调用记录仍保留，发现与加载不进入生活记忆。参数、错误和兼容约定见[摘要查询与按需加载](./插件运行时接入.md#摘要查询与按需加载)。

| 文件 | 用途 |
| --- | --- |
| [插件系统开发指导](./插件系统开发指导.md) | 产品定位、Plugin/Skill/Tool 职责、开发顺序和最低验收要求 |
| [工具层准入契约](./工具层准入契约.md) | 确定准入单位、最低要求与主体责任 |
| [Skill 与 Tool 分类及按需加载约定](./Skill与Tool分类及按需加载约定.md) | 区分方法与操作，规定简短描述、选择、加载和依赖引用 |
| [Tool 能力包声明模板](./能力包声明模板.md) | 规定 Tool 作者交付文件、字段、调用和结果语义 |
| [capability.schema.json](./capability.schema.json) | 检查 Tool 格式 1 的声明结构和部分条件，不用于验证纯 Skill |
| [plugin.schema.json](./plugin.schema.json)、[skill.schema.json](./skill.schema.json) | Plugin 安装清单与纯方法 Skill 的声明结构 |
| [插件运行时接入](./插件运行时接入.md) | plugins.json、授权、结果适配、会话回复绑定及取消/回执语义 |
| [观察源与聊天插件接入](./观察源与聊天插件接入.md) | 主动查询、可选 JSONL source、账号身份、监听游标及真实回执 |
| [source.schema.json](./source.schema.json) | agent-observation-1 来源进程声明 |
| [受控聊天示例](./examples/controlled-chat/README.md) | 可运行的本地聊天日志、宿主配置生成与可选监听 |
| [LX Music 通用插件](../../plugins/lx-music/README.md) | 现有音乐工具＋Skill 的可执行接入样板、角色配置、macOS 验证与 Windows 实测步骤 |
| [准入检查规则与示例核对](./准入检查规则与示例核对.md) | 区分结构检查、语义核对和当前运行条件 |
| [现有工具契约适配验证](./现有工具契约适配验证.md) | 当前内部工具、外部插件、兼容边界和验证覆盖 |
| [examples](./examples) | 脚本、通用 CLI、MCP 及受控聊天作者示例 |
| [verify_contract.py](./verify_contract.py) | 可重复运行的有限验证，包含正反例与临时数据测试 |

## 复现验证

从仓库根目录执行，依赖只安装到临时目录，不修改项目依赖：

```sh
contract_deps="$(mktemp -d)"
python3 -m pip install --target "$contract_deps" jsonschema==4.23.0
PYTHONPATH="$contract_deps" python3 contracts/tools/verify_contract.py
```

需要 Python 3 和 Git。脚本输出逐项 JSON 结果；所有检查符合预期时退出码为 0，反例未被拒绝、正例未通过或执行异常均使验证失败。2026-10-17 增加观察源与受控聊天验证后结果为 **94/94**；原有 Tool/Plugin/Skill 验证保留。

2026-10-19 复验当前脚本为 **96/96**，复用已有临时 jsonschema 环境，没有新增项目依赖。摘要分页、游标失效、候选轮转和旧插件兼容由 C++ PluginRuntimeTests、ActivitySelectorTests、AutonomousChatTests、LxMusicPluginTests 覆盖。

Python 验证包括声明结构、示例中的简单引用、业务参数正反例、参考计数脚本、临时 Git 仓库以及已有歌单脚本的查询函数。歌单查询只读取验证脚本创建的临时 SQLite 数据。该脚本不连接 MCP、不启动 LX Music 或桌宠，也不读取实际用户数据库。加载器和真实 stdio MCP 流程另由 C++ PluginRuntimeTests 的临时插件与受控服务验证。

观察源验证还会启动本地 controlled-chat 进程，以独立临时日志核对分页读取、实际发送与回执、JSONL 来源、ack、游标重放及宿主配置生成。完整主体的等待、阅读和恢复链路由 C++ AutonomousChatTests 验证；这些测试均不登录真实 QQ 账号。

引用核对仅支持这些示例中的直接配置引用和顶层属性映射，不是通用准入检查器；调度参数组合用例是用于验证标准表达能力的精简样例，不代替 C++ 调度器测试。脚本与 CLI 测试按示例约定直接执行，也不构成宿主加载器实现。

## 版本管理边界

正式工具契约及验证附件在本目录维护，主体演进设计在 `contracts/agent/` 维护。`.evo/` 继续保存本地过程记录；已撤回的系分草稿不属于本契约。2026-10-15 的契约验证未改变记忆设计或实施工具解耦；2026-10-16 的自主 Agent 设计进一步定义人物记忆与活动运行时，进度以该设计引用的 impl-plan 为准。
