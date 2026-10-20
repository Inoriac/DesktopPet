# QQ 插件实现审查

## Task 1 — 读取与观察

- 审查账号验证、会话路由、SQLite 事务、分页/水位、source 控制与输出限制。没有修改宿主协议、人物/记忆或自主调度器。
- 主审要求修正登录响应前推送丢失；短命令默认不缓存无关消息；仅 source 有界暂存并在身份验证后交付。畸形 URL/消息类型不再崩溃，list 使用短摘要。
- NapCat 自己发出的私聊历史用可信查询目标，实时无目标消息不猜路由。signed message_id、缺原始时间、文本截断、媒体占位已覆盖。
- RED：初始模块缺失；首轮子进程测试暴露 Python daemon 持有 BufferedReader 锁导致 stop 退出 -6，改原始 stdin 后 GREEN。
- 主代理复跑 QQAdapter 17/17；既有 PluginRuntime、ExternalObservation、PersonDirectory 3/3 目标通过。只连接本地假 OneBot 服务。
- Wave 核查：单一实现者无冲突，设计测试场景均覆盖，测试位于既有 tests/ 布局，锚点与 context 留痕一致，git diff --check 通过。按既有用户授权提交并进入 Task 2。
