# LX Music 通用能力接入实现计划

系分文档：[LX Music 通用能力接入](./LX-Music通用能力接入-桌面端-系分.md)。本文件是进度唯一来源。授权：用户要求完成 macOS 上可做工作，沿用自主推进、逐阶段 commit。

## Task 1: 音乐工具与技能能力包

- 关联设计: §3.2
- 状态: [x]
- Wave: 1
- 依赖: 无
- 文件: plugins/lx-music/{adapter.py,plugin.json,tool.json,skill.json,SKILL.md,USAGE.md}；tests/test_lx_music_adapter.py；CMakeLists.txt（Python 测试注册）。
- 测试用例: sqlite_ordered_queries_read_only、missing_database_no_create、unknown_and_empty_playlist、parameterized_id、http_status_and_lyric_no_launch、http_invalid_timeout、play_pause_volume_verified、write_disconnect_once_unknown、skip_acknowledged_once、launch_and_scheme_argv、search_mismatch_unknown、unplayable_playlist_no_dispatch、diagnose_read_only、input_bounds_and_json、package_operation_coverage。
- 覆盖来源: §3.2.7，测试放置参照 tests/test_launcher_config.py 与 tests/test_plugin_runtime.cpp。

验收：父级代码审查通过；完整构建及 58/58 CTest 通过（105.50s）。审查后的来源 ID 路径兼容修复增加先 RED 后 GREEN 用例，23/23 专用 Python 测试通过（15.758s）；无其他源代码变化。三份包 schema 校验通过，原契约 94/94。Wave 五项（冲突、回归、行为覆盖、测试位置、锚点留痕）通过。日志 /tmp/lx-task1-{build,full-ctest,scheme-green}.log，报告 /tmp/lx-task1-report.md。

## Task 2: 角色配置、运行时验证与 Windows 交接

- 关联设计: §3.3
- 状态: [ ]
- Wave: 2
- 依赖: Task 1
- 文件: plugins/lx-music/{configure.py,README.md}；tests/test_lx_music_configure.py；tests/test_lx_music_plugin.cpp；tests/fixtures/lx_music_backend.py；CMakeLists.txt；contracts/tools/README.md。前序文件发现缺陷可在父级审查批准后修复并回归。
- 测试用例: configuration_preview_no_write、apply_preserves_unrelated_and_backup、apply_idempotent、invalid_configuration_unchanged、autonomy_off_read_all、apply_failure_rollback、runtime_discover_load_skill_and_tools、runtime_command_http_and_sqlite、runtime_local_and_internal_permissions、runtime_external_denied、runtime_invalid_disabled_stale_no_write、runtime_unknown_single_dispatch。
- 覆盖来源: §3.3.7；C++ 使用实际 PluginRuntime 与包，不使用既有内置音乐工具。
