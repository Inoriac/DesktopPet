# Launcher QQ 风格聊天

## 需求来源
用户要求先检查最新 Windows 验证提交 dd6c7cd；没有阻塞问题后重构 launcher 聊天页为 QQ 私聊风格，上滑按页加载更早记录，提供聊天记录搜索。用户离线，明确授权自行决定、持续实现并分批 commit。

## 工作约定
- 工作区 /Users/huang/Desktop/DesktopPet；单仓库，无 AGENTS.md。evo CLI 不可用，使用本目录。
- 使用 evoloop-gen-tech-design / evoloop-code-impl。用户的持续推进授权覆盖流程中常规方案、分批与阶段确认；保持串行子任务、TDD、主代理审查和验证。
- 不运行付费真实模型或控制真实音乐/QQ，不读取个人聊天内容作截图；测试与视觉验收使用合成消息和临时文件。
- cmake/ctest: /Users/huang/.qt-build-env/bin/；build/；Python: launcher/.venv/bin/python；QT_QPA_PLATFORM=offscreen。
- 测试统一 tests/test_*.cpp / tests/test_*.py，已有 ChatUiModelTests、ChatHistoryTests、LauncherChatTests、LauncherChatTransportTests。

## Windows 提交审查
- 已读两份验证报告，检查 ModelRouter schema 提示、PluginRuntime 文件关闭、受控聊天跨平台 stdin/路径、SQLite 关联及测试解释器变更。没有发现阻塞本次 UI 工作的问题。
- macOS 全构建通过；ModelRouterTests / PluginRuntimeTests / ExternalObservationTests / LxMusicPluginTests / LxMusicAdapterTests 5/5（40.99 秒）；契约 96/96。Windows 本机原始日志未随提交入库，报告中的实际听感/长期运行限制予以保留。
- 正式旧模型名在报告中返回 unknown provider，部分角色尚为占位名；隔离验收成功不代表正式配置可用。本次不改变用户模型或凭据。
- LX 2.12.2 status 缺音量字段，set_volume 返回 unknown 是已说明的兼容限制；不将其伪报为完成。

## 决策记录
- [Task 1 sanity check] 实际模型测试名 ChatUiModelTests / chat_ui_model_tests；ConversationStore.loadHistory 使用写锁事务，新查询使用局部只读连接和读取事务，不占用写锁。签名核实通过。
- [Task 1 接口补充] historyFailed(action,requestToken,detail) 关联查询错误（包括超时/断线），防止旧搜索失败污染新搜索；保留普通 operationFailed 两参数接口。
- [Task 1 可见消息边界] 查询与页数只统计 user/assistant 非空正文，内存快照也保留 pending/streaming；system 仅在独立 diagnosticMessages（最多20条）提供给诊断区。先筛选再分页，覆盖交错 system/空错误占位；原 load 行为不变。server 认证后剥离 sessionToken 再校验查询参数。
- [code-impl Task 1 sanity check] §3.2 存储/query、model recentHistoryPage/requestHistory、LauncherChatCallbacks::queryHistory、Python request_history/search_history/historyFailed 实际签名已核实，与修订后的设计一致。
- 保留 PySide6/Fluent、现有认证管道和任务操作；不更换 UI 框架、不增数据库。
- 目前主体 snapshot 固定最近 120 条，launcher 仅本地展开。改为最近 40 条实时窗口和独立分页/搜索查询，历史不会随轮询被删掉。
- 采用 ID 游标，数据库内按现有 rowid 插入顺序翻页；同一时间戳不会丢消息。仅访问当前 profile 的 local_owner。
- 搜索按字面子串，中文可用，英文忽略大小写；不解释 %/_ 为通配符；结果分页，点击读取上下文并定位，保留返回最新入口。

## 设计偏差
无。

## 待回溯设计的发现
无。
