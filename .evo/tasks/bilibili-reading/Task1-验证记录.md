# Task 1：B 站查询与字幕阅读验证

父级 CR：已逐项阅读适配器、真实后端、清单与操作说明，复跑 29 项测试，并核实真实 CLI 搜索/第二 P/评论成功、匿名字幕鉴权错误；没有阻断问题。按持续有效的分阶段提交授权进入 Task 2。

关联设计：§1、§3.2。日期：2026-10-19。实现前已读取系分、impl-plan、context，确认五个读取操作、包外状态、按需文本分页与 format 1 约束。没有编辑用户配置、调用用户账号、运行全项目构建或提交代码。

## 实现与 sanity check

- `backend.py` 采用真实 `bilibili-api-python==17.4.1` 的异步 `search.search_by_type`、`Video.get_info/get_subtitle`、`comment.get_comments`。临时真实环境通过 inspect 核实签名；上游依据为固定提交 `5a2081bfcd67a4a3e9f24948a8449d184eb70ece` 的 bilibili-mcp 读取函数。
- `Video.get_info.pages[part-1]` 取得目标分 P 的 CID、时长，非法 part 拒绝；没有使用全视频默认 CID 代替第二 P。
- 已采用父级只读实测形状：评论可能只返回少数 replies，但 `page.count` 仍有大量后续内容，因此由 `page.num * page.size < page.count` 判断下一页；无登录的 `CredentialNoSessdataException` 归为 `AUTH_REQUIRED`，不当作没有字幕。
- 额外核实真实库使用 `request_settings.set_timeout` 和 `set_wbi_retry_times`。`utils/network.py:2357` 的计数表示总尝试次数，设为 1 可关闭自动重试；adapter 另以 asyncio.wait_for 限定整轮读取期限。
- `CapabilityExecutor::interpret` 使用宿主 resultAdapter 的 /state、/data；普通 sanitizer 字符串上限 2000 UTF-16、payload 6000 UTF-16。本实现 data 上限 3800、单字符串 <=1800，并测试带实际 stage/data 包装后的体积。
- 清单 command 第一个参数为 `-B`，adapter 在导入同包模块前设置 `sys.dont_write_bytecode=True`。状态、缓存写包外，子进程用例确认不新增包内 pyc，避免影响 PluginCatalog 摘要。
- 两份上游 MIT 许可已完整保留，THIRD_PARTY_NOTICES 说明来源及改动；没有导入平台写操作或 DSH 调度器。

## TDD 记录

1. 先新增 `tests/test_bilibili_adapter.py`，首次运行失败：`ModuleNotFoundError: No module named 'plugins.bilibili'`，证据 `/tmp/bilibili-task1-red.log`。之后实现读取、校验、缓存、CLI。
2. 网络分块读取测试先失败：首次 StreamReader.read 只返回部分 JSON；修复为受总量限制的完整分块接收。证据 `/tmp/bilibili-task1-stream-red.log`。
3. Python 3.13 用例发现 asyncio.TimeoutError 是 OSError 的别名，原异常分支错误归类为 STATE_UNAVAILABLE；移除该全局拦截，网络错误统一映射。证据 `/tmp/bilibili-task1-python313-red.log`。
4. 新增损坏快照恢复、读取中切换账号、缓存写失败和转义文字保真用例，先得到 3 failures + 1 error，再修复为：第一页原子重写同名内容快照、凭证变化拒绝发布、局部状态错误映射、按 JSON 体积继续拆段。证据 `/tmp/bilibili-task1-boundary-red.log`。

最终命令：

```sh
/usr/bin/python3 -B tests/test_bilibili_adapter.py -v
/private/tmp/desktop-pet-bilibili-dev-2rnhj2vq/.venv/bin/python -B tests/test_bilibili_adapter.py -v
```

**Python 3.9.6：29/29；Python 3.13.14：29/29 通过。** 日志分别为 `/tmp/bilibili-task1-final-python39.log`、`/tmp/bilibili-task1-final-python313.log`，约 1.3 秒（含真实 1 秒操作超时验证）。没有账号、网络或第三方模型参与这些受控用例。

使用既有临时 jsonschema 依赖（未安装新包）执行 Draft202012Validator，对 plugin/tool/skill 分别应用仓库 plugin.schema.json、capability.schema.json、skill.schema.json：**3/3 通过**。`git diff --check` 通过。

## 关键行为映射

| 设计场景 | 对应用例 |
| --- | --- |
| status 缺依赖、不联网不写状态 | test_status_missing_dependencies_does_not_call_network_or_write |
| search 默认值、排序、真实来源和 Unicode | test_search_normal_defaults_and_unicode；test_search_passes_page_size_and_order_and_removes_html |
| 正确第二 P / 非法 P | test_second_part_info_and_unknown_part；test_backend_second_part_cid_and_duration；test_backend_unknown_part_is_not_first_part |
| 评论空结果与失败、分页真实计数 | test_comments_empty_is_not_failure；test_comments_page_count_controls_more_not_short_reply_list |
| 字幕中文优先、毫秒时间戳 | test_subtitle_uses_selected_cid_and_chinese_timestamps |
| 字幕全文多页还原、emoji/转义不丢字、体积 | test_transcript_pages_preserve_all_text_timestamps_and_source；test_many_escaped_control_characters_are_read_without_loss |
| 无字幕与登录/网络失败区分 | test_no_subtitle_is_distinct_from_network_failure；test_credential_requirement_is_not_an_empty_transcript；test_errors_are_stable_and_do_not_expose_remote_messages |
| 错游标、视频/part/source/账号绑定 | test_cursor_rejects_wrong_video_part_source_and_account；test_deleted_or_corrupt_cache_asks_for_first_page；test_account_change_during_read_cannot_publish_to_new_account |
| 缓存去重与凭证不落入内容、ASR 共用快照 | test_same_content_reuses_snapshot_and_does_not_store_credentials；test_local_asr_reads_published_snapshot_without_network |
| CLI UTF-8 / 包内不生成 pyc / 输入错误 | test_cli_unicode_json_and_no_package_bytecode；test_cli_malformed_json_is_a_failed_envelope |
| 清单和参数禁止平台写入、配置路径 | test_manifest_has_only_read_commands_and_matching_skill_dependencies；test_bad_arguments_and_platform_writes_never_dispatch；test_settings_defaults_and_invalid_paths |
| 状态落盘失败、真实操作超时、简介裁剪 | test_cache_write_error_is_a_state_error_not_empty_subtitles；test_full_operation_timeout_is_bounded；test_public_text_is_bounded_and_marks_truncation |

## 后续 Task 使用的内部接口

所有下列 settings 参数使用 `validate_settings` 返回的规范化 dict。它接受全部 §1.2 字段并补默认值，Path.resolve 规范绝对路径；stateDir 必填，credentialPath 默认 stateDir/credential.json。

| 签名 | 返回与注意事项 |
| --- | --- |
| `validate_settings(value) -> dict` | 配置错误抛 AdapterError（BilibiliError 的别名），字段 asrPython/asrModelPath 缺省为 None |
| `load_settings(path) -> dict` | 读取 <=64 KiB UTF-8 JSON 后规范化 |
| `run(operation, arguments, settings, backend=None) -> dict` | 同步入口；completed/failed envelope，backend 注入仅代码级测试，不在 CLI 中暴露 |
| `main(argv=None, backend=None) -> int` | 读取 stdin UTF-8 JSON，输出单 envelope，业务失败返回 0；调用方可在 tests/fixtures 导入并注入后端 |
| `envelope(state, data=None, error=None) -> dict` | error 需要 code/message 属性；返回 `{state,data}`，ASR 可以再添加 receipt/tracking |
| `credential_identity(settings) -> str` | 当前凭证文件内容的 SHA-256；缺文件时为空内容的指纹，绝不输出凭证内容 |
| `atomic_json(path, value)` | 同目录临时文件 + os.replace；状态错误抛 STATE_UNAVAILABLE，可供任务文件复用 |
| `save_transcript(settings, video, source, segments, language="", reason="") -> str` | 发布快照及最新索引，返回 64 位十六进制 snapshot ID；video 必需 bvid/part/cid，title 可选；segments 为 `{startMs:int,endMs:int,text:str}`，长段保真拆分。source 必须 platform_subtitle/local_asr；空 local_asr 默认 no_speech |
| `read_transcript_page(settings, bvid, part, source, cursor=None) -> dict or None` | 返回有界 data，包含来源/身份/segments/hasMore/nextCursor；无最新索引返回 None，坏快照/游标抛 INVALID_CURSOR。正常业务从 adapter.run(read_transcript,...) 调用 |
| `BilibiliBackend(settings).video_info(bvid, part)` | async，返回 bvid/aid/cid/part/partCount/title/partTitle/durationSeconds/description/author，核实对应 part |
| `BilibiliBackend(settings).search(keyword,page,limit,order)` | async，返回 entries/total/hasMore，分页由真实 API page_size=limit 控制 |
| `BilibiliBackend(settings).comments(bvid,page,limit)` | async，返回 entries/total/hasMore/truncated；样本裁剪不承诺导出所有评论 |
| `BilibiliBackend(settings).transcript(bvid,part)` | async，返回 video/availability/reason/language?/segments，异常向上抛；由 run 映射稳定错误 |
| `backend.read_credential(settings) -> dict` | 供人工设置/下载代码复用；JSON 字段为 Cookie 名到字符串，缺文件返回 {}；凭证错误不回显内容 |

ASR worker 发布前应核实 credential_identity 与受理时身份一致，避免转写途中换账号串缓存。Task 2 新增 ASR 清单依赖应保留其可选性，防止未授权 start_transcription 时阻断原五项基础阅读 Skill。

本 Task 未运行真实联网字幕下载或转写。父级已另行完成实际 adapter CLI 匿名烟测：search 返回两条、video_info(part=2)、comments 返回三条，均 completed；字幕 AUTH_REQUIRED；四次退出 0、stderr 为空，详见 context。没有发现需修改 §3.2 接口的偏差；网络库 request_settings、包摘要与评论形状为实现锚点细化，已报告父级回填。
