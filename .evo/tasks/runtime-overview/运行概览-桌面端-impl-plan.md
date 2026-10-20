# 运行概览实现计划

关联设计：[运行概览-桌面端-系分.md](运行概览-桌面端-系分.md)。进度以本文件为唯一来源，用户已授权自主执行及阶段 commit。

## Task 1：角色用量与运行统计
- 关联设计：§3.1
- 状态：[x]
- Wave：1
- 依赖：无
- 文件：include/{ai_types,statistic_types}.h、statistic/statistic_manager.*、core/ai/model/model_router.*、core/ai/llm/{llm_chat_service,llm_chat_model_client,openai_compatible_client,anthropic_messages_client}.*、core/ai/ai_brain.cpp、main.cpp；tests/test_{statistic_manager,model_router,llm_chat_service,anthropic_messages_client}.cpp；必要的 CMakeLists.txt 测试注册。
- 测试：attempt_rollups_consistent、fallback_and_repair_count_once、duplicate_completion_dedupes、reservation_denial_not_counted、cancelled_attempt、missing_vs_reported_zero_usage、anthropic_cache_total、profile_restart_rename_isolation、legacy_import_without_fake_details、runtime_checkpoint_not_double_counted、initialization_clears_previous_data、bounded_recent_history、persistence_failure_visible。

## Task 2：认证运行快照与日志维护
- 关联设计：§3.2
- 状态：[x]
- Wave：2
- 依赖：Task 1
- 文件：core/ai/chat/launcher_chat_server.*、core/ai/runtime/runtime_overview.*、ui/petwindow.{cpp,h}、core/ai/ai_brain.{cpp,h}、core/ai/ai_call_logger.*；tests/test_launcher_chat_server.cpp、tests/test_runtime_overview.cpp、tests/test_ai_call_logger.cpp；CMakeLists.txt。
- 测试：authenticated_overview_without_history、overview_auth_and_unsupported、query_does_not_change_revision、emotion_disabled_no_fake_state、runtime_fields_from_real_inputs、logger_rotation_and_oversize、logger_write_failure。

## Task 3：Launcher 运行概览与调用记录
- 关联设计：§3.3
- 状态：[ ]
- Wave：3
- 依赖：Task 1、Task 2
- 文件：launcher/pages/statistics_page.py、launcher/statistics_data.py、launcher/launcher_chat_client.py、launcher/main.py；tests/test_launcher_statistics.py、tests/test_launcher_chat_transport.py；CMakeLists.txt、launcher/README.md、本任务验收文档和 visual-qa 脚本/截图。
- 测试：overview_transport_coalesces_and_user_priority、dashboard_live_ranges_and_emotion、offline_load_no_live_mood、profile_switch_and_stale_response、hidden_page_no_poll、unsupported_core_stops_poll、recent_filters_and_pages、light_dark_narrow_layout；既有 LauncherChatTests/LauncherChatImageTests/LauncherChatTransportTests。

## 每 Wave 校验
检查冲突、相关新旧测试、测试场景覆盖、tests/ 布局、设计锚点及 context 留痕。审查通过后提交；最终补充实现总结和本地/Windows 验收边界。
