# QQ 插件上下文

## 需求来源
用户要求借鉴 https://github.com/Mai-with-u/MaiBot ，改进为适合 DesktopPet 的插件。延续用户对后台 QQ、通用工具契约、自主选择、人物关系/范围隔离与分阶段 commit 的要求。实现不测试真实私人账号或向外发消息。

## 决策记录
- [code-impl Task 3 sanity check] 四份清单、精确 objectArguments、observation/delivery resultAdapters 与当前 PluginRuntime 对齐；实际子进程/网关验证私聊和群聊归档、发送、权限及未知结果。source 与自主授权独立，发送仅限匹配外部活动；没有任意主动开聊工具。全项目构建和本地 76/76 CTest 通过，既有 WebCommandTests 环境阻塞明确排除；真实平台测试交接，不修改个人配置。
- [code-impl Task 2 sanity check] send/receipt 的 execute 分派、OneBotClient.call 的截止时限、Journal 的账号绑定和新 attempts API、capabilityDeliveryReceipt 的真实 id/content/time 要求一致。实际 API 状态归因修正见设计偏差，已复测。
- [code-impl Task 1 sanity check] ObservationStore::ingest/normalize 的文本、身份、时间和历史标志映射；CapabilityExecutor::execute 的 stdin=call.arguments；source start/ack/stop 均核对一致。细化 OneBotClient.capture_events 与 Journal.ingest.requested_chat 已回写 §3.1，正文不改变现有宿主契约。
- [协议证据] OneBot v11 get_msg 标准未要求目标字段；NapCat `450fb0429a064e3418e0c698b1b4d78324101929` 的 packages/napcat-onebot/api/msg.ts 初始化 user_id=sender，私聊无 peer 字段。§3.1/3.2 已据此细化：历史自身消息用可信查询目标，私聊发送回执必要时用目标限定历史验证同消息。source 无目标自身私聊不猜路由。有符号 message_id 与正数 QQ 账号分开处理。仅阅读源码证据，不连接实际 QQ。
- [用户最新范围] 不测试实际 QQ 是否连通；真实 QQ 登录、连通与收发的测试文档交接其他人。实现期间仅本地假服务及代码回归，交付不能声称真实 QQ 已通过。
- [设计细化] settings.allowedConversations 使用会话键字符串数组；tokenFile 可省略；requestTimeoutSeconds 默认 10（1–25）。已核实宿主 objectArguments 是精确值比较，不是允许值数组；resultAdapters 键为 entry/operation，设计锚点已更正。
- 采用 evoloop-gen-tech-design 与 evoloop-code-impl 工作流。用户先前“你看着往下做吧，每做一个阶段就 commit 一下”及离线自主推进授权持续有效，不再逐阶段询问批准。保持串行 subagent、TDD、review 与验证。
- 当前宿主只支持 text/reply 外部观察。首版用诚实媒体占位，QQ 图像理解/上传不在本次文本接入中宣称完成。复用既有自主机会、冷却及身份/记忆，插件不另建主动回复调度器。
- 研究仓库只读放在 `/tmp/desktop-pet-maibot-review.pEfjaV/`。仅借鉴协议与架构，原创实现，不复制 GPL 代码到本项目。
- Python 3.9.6 可用；测试依赖使用包外 `/tmp` 独立环境，不修改角色账号配置。Qt 测试默认环境没有 websockets 时 QQ 集成需显式传 QQ_TEST_PYTHON；其他测试沿用原 Python。
- 测试布局参照 plugins/bilibili 与 tests/test_bilibili_{adapter,configure}.py / test_bilibili_plugin.cpp。没有 AGENTS.md。

## 设计偏差
- [Task 3 结果预算回溯，已解决] 宿主先裁剪单字符串到 2000，再将超 6000 字符整包省略。原 read/list 大页不可用于模型，主代理以 18168 字符读页和不支持 list 游标的两条 RED 复现；补约 3500 字符有界页、list allowlist 游标，单条原文保留给归档，超长内容由宿主缩短展示。§3.1/6 已回写，C++ 补真实 sanitizer 验证。
- [Task 2 协议回溯，已解决] Task 1 将所有非 ok/0 响应归明确拒绝，无法正确区分 async/畸形响应。主代理先以 6 个变体复现误分类，再细化 §1.5、修正 transport，发送层将此类情况保留 unknown。未扩大宿主协议。

## 待回溯设计的发现
无。
