# QQ 插件实现审查

## Task 1 — 读取与观察

- 审查账号验证、会话路由、SQLite 事务、分页/水位、source 控制与输出限制。没有修改宿主协议、人物/记忆或自主调度器。
- 主审要求修正登录响应前推送丢失；短命令默认不缓存无关消息；仅 source 有界暂存并在身份验证后交付。畸形 URL/消息类型不再崩溃，list 使用短摘要。
- NapCat 自己发出的私聊历史用可信查询目标，实时无目标消息不猜路由。signed message_id、缺原始时间、文本截断、媒体占位已覆盖。
- RED：初始模块缺失；首轮子进程测试暴露 Python daemon 持有 BufferedReader 锁导致 stop 退出 -6，改原始 stdin 后 GREEN。
- 主代理复跑 QQAdapter 17/17；既有 PluginRuntime、ExternalObservation、PersonDirectory 3/3 目标通过。只连接本地假 OneBot 服务。
- Wave 核查：单一实现者无冲突，设计测试场景均覆盖，测试位于既有 tests/ 布局，锚点与 context 留痕一致，git diff --check 通过。按既有用户授权提交并进入 Task 2。

## Task 2 — 发送与回执

- 主审核对单次 send、账号/目标/引用校验、派发前 pending 提交、远端 ID 先存后查、派发后存储故障返回 unknown。没有正文去重或自动发送重试。
- 真实私聊 get_msg 无目标字段时使用限定会话历史，明确错误目标不能被覆盖；SnowLuma 群聊 seq=0 保持未知。receipt 只查询插件凭证，不改宿主旧派发账本。
- RED：新增发送/回执场景因 UNKNOWN_OPERATION/缺 attempts API 失败。子代理发现 transport 将 async/畸形状态误认明确拒绝，主代理另以 6 个歧义变体复现后修复并回写 §1.5。
- GREEN：QQDelivery 24/24，QQAdapter 18/18；主代理复跑通过。涵盖实际 command 与 fake 服务子进程，未访问真实 QQ。
- Wave 核查：仅 Task 2 文件及已明确回溯的 transport/对应回归；各设计场景有行为测试，目录/锚点一致，git diff --check 通过。按用户授权进入打包与交接。

## Task 3 — 打包、配置与宿主集成

- 核对四份清单、按需加载文档、配置预览/备份/原子合并与包外安装。配置只处理指定角色；source 与自主工具授权分别控制，不更改宿主自主总开关。
- 本地主人/内部活动仅授只读操作与 Skill；外部会话的 read/send/receipt 绑定精确 chat_id，不授 list。账号身份、来源和返回值适配复用既有契约，没有新增 QQ 专属宿主分支。
- 修复主审发现的大页结果超宿主预算问题：read 按体积分页、list 增加绑定白名单的游标；C++ 集成通过真实 ToolResultSanitizer 验证页面与游标可见，原文仍供归档。
- 测试覆盖真实 command 子进程与宿主网关：私聊/群聊读取归档、完成回执、跨会话拒绝、插件禁用、未知结果保留与不重发，以及 source 重放去重。测试夹具临时 QJsonObject 的 QJsonValueRef 生命周期错误已修复；生产代码未涉及该问题。
- GREEN：QQAdapter 20/20、QQDelivery 24/24、QQConfigure 9/9；QQPlugin QtTest 8 项通过、0 失败、0 跳过。没有 websockets 的环境明确 skip 相关测试，不将缺依赖记作通过。
- 最终验证：全项目构建通过；本地 CTest 76/76 目标通过；公共契约校验 96/96，QQ 四份 JSON 清单 schema 校验通过。WebCommandTests 因既有 macOS Qt 6.8.3 QProcess/QtGui 辅助进程被 AMFI/XProtect 终止的问题排除，不计作通过。
- Wave 核查：串行工作无文件冲突，测试布局/设计锚点/context 一致。真实 QQ 与 Windows 测试未执行；README 与验收交接包含部署步骤、验收用例、失败证据与问题报告模板。
