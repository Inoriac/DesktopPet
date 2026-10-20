# QQ 插件实现计划

系分文档：[QQ插件-桌面端-系分.md](QQ插件-桌面端-系分.md)。此文件为进度唯一来源。每阶段串行实现、主代理审查、回归后提交；用户已授权自主推进和分阶段 commit。

## Task 1: 授权会话读取与可靠观察源
- 关联设计：§3.1
- 状态：[ ]
- Wave：1
- 依赖：无
- 文件：plugins/qq/{common,transport,journal,adapter,source}.py、requirements.txt；tests/test_qq_adapter.py、tests/fixtures/qq_backend.py。
- 测试：read_authorized_history、normalize_private_with_group_id、normalize_media_missing_time、journal_replay_dedupe、query_does_not_ack、source_process_replay_ack_stop、source_backpressure_eof、wrong_account_rejected、history_unsupported_local_only、source_reconnect。

## Task 2: 文本发送与可追踪回执
- 关联设计：§3.2
- 状态：[ ]
- Wave：2
- 依赖：Task 1
- 文件：plugins/qq/delivery.py、adapter.py、journal.py（必要的 attempts API）；tests/test_qq_delivery.py、tests/fixtures/qq_backend.py。
- 测试：send_private_and_group_once、send_snowluma_confirmed_seq、send_reply_and_literal_cq、confirm_wrong_target_sender_time_unknown、send_timeout_no_retry、receipt_after_restart、receipt_no_remote_id_unknown、send_denied_no_network、receipt_scope_rejected。

## Task 3: 插件打包、角色配置与宿主验收
- 关联设计：§3.3
- 状态：[ ]
- Wave：3
- 依赖：Task 1、Task 2
- 文件：plugins/qq 剩余 manifest/tool/source/skill/setup/configure 与文档；tests/test_qq_configure.py、tests/test_qq_plugin.cpp、tests/fixtures/qq_backend.py；CMakeLists.txt；contracts/tools 文档索引。
- 测试：preview_no_write、apply_preserves_unrelated_and_idempotent、invalid_or_concurrent_config_rejected、precise_grants_and_identity、runtime_load_read_archive_send_receipt、runtime_scope_and_disable、source_autonomy_independent、package_immutable。

## Wave 检查
每 Wave：文件冲突；新旧相关测试；上述行为覆盖；tests/ 命名/布局一致；设计锚点与 context 留痕。最终执行全量可用回归，记录真实后端待验收范围。
