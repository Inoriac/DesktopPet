# Task 2：可选本地语音转写验证

父级 CR：已阅读受理、后台监督、音频下载、转写、缓存发布、清单和说明；修正文档中 accepted 映射适用范围。父级合并运行 45/45 Python 测试通过，并验证真实引擎的完整后台链路。无阻断问题，按持续有效授权进入 Task 3。

关联设计：§1、§3.3，依赖 §3.2。日期：2026-10-19。Task 2 实现完成，交父级 CR；未 commit、未修改计划进度，未编辑用户配置或使用账号。

## 实现与 sanity check

- 已读取 §3.3 全 7 子节、§1、§3.2 接口/缓存约束、impl-plan、context 与 Task 1 验证记录。文件范围依照计划，额外只做父级明确要求的共享解释器路径/缓存身份细化。
- 在既有临时 Python 3.13 环境 inspect 了 `WhisperModel(model_size_or_path,device,compute_type,local_files_only,...)`、`transcribe(audio,vad_filter,...)`、`YoutubeDL(params,...)`、`extract_info(url,download=True,...)` 与 CookieJar；签名符合设计。实际 `BiliBiliIE._real_extract` 使用 `?p=N` 选择分 P，单视频 ID 为 BV 或 BV_pN。固定传入显式 p，下载前核实 ID 与时长，拒绝 interactive/playlist/multi_video，避免读错分 P。
- `transcription.py` 实现受理、持久回执和状态查询：依赖预检与分 P 查询共享整次请求期限；原子目录短锁只覆盖任务发布，同 BV/part/CID/账号/模型的运行请求去重。已完成快照可复用；已知失败后的新 `start_transcription` 才会新建任务，查询与 worker 不自动重试。
- job 文件保存固定输入、账号指纹、设置、开始/截止时间和终态。job_id 为 32 位小写十六进制；模型输入不能传路径、URL 或脚本。损坏/丢失记录明确 ASR_UNKNOWN；截止时间过后仍无终态的任务表现 unknown，不永久显示 running，不直接重开重复任务。
- `asr_worker.py` 使用固定 supervisor → engine 子进程，超时由 subprocess.run 终止 engine 并等待退出；不是同步占用宿主普通调用。音频下载使用 yt-dlp bestaudio/best、零重试、无用户缓存，Cookie 仅放内存 CookieJar。PyAV 直接解码下载音频，不调用外部 FFmpeg 转码；faster-whisper 使用 CPU/int8、local_files_only=True、vad_filter=True。
- 引擎输出保留原时间段，supervisor 成功后复用 save_transcript。新增可选 `expected_account`：发布时只读一次当前指纹，校验和构造缓存身份使用同一个值，杜绝检查后切号时误写到新账号缓存；平台字幕也复用此接口，删除重复检查。空语音完成并标记 no_speech。
- 所有 Python 入口/子进程使用 -B 或 sys.dont_write_bytecode；临时音频在 jobs/<id>/work，终态清理仅针对该目录。凭证、签名 URL、引擎原始错误不进入回执、状态或日志。
- 父级发现实际 venv/bin/python 是符号链接，Path.resolve 会丢失环境。asrPython 改为保持入口符号链接的绝对路径，state/model 目录身份仍 resolve；已加回归。
- tool.json 现在有七个操作，只有 start_transcription 为 write；ASR 两操作在 skill.json 独立声明为 optional，基础五项继续 required。无主系统 C++ 生产变更，无调度器、平台发言或强制报告。

## TDD 记录

1. 先写受理、并发去重、真实后台进程、分页、超时/失败、账号变化、回执路径等 12 项测试。首次 RED：`ImportError: cannot import name 'transcription'`，日志 `/tmp/bilibili-task2-red.log`。随后实现模块、入口和 fixture，Python 3.9 首轮 12/12 通过。
2. 补真实依赖导入失败、受理总期限、账号检查/缓存 key 单次指纹与损坏状态测试。损坏 JSON 对象缺少身份字段时先得到 BACKEND_ERROR，RED 日志 `/tmp/bilibili-task2-corrupt-red.log`。修复 load_job 的必要字段检查后明确 ASR_UNKNOWN，16/16 通过。
3. 清单增加 optional ASR 回归，Task 1 29 个读取/分页/CLI 测试全部继续通过。

最终验证命令：

```sh
/usr/bin/python3 -B tests/test_bilibili_transcription.py -v
/private/tmp/desktop-pet-bilibili-dev-2rnhj2vq/.venv/bin/python -B tests/test_bilibili_transcription.py -v
/usr/bin/python3 -B tests/test_bilibili_adapter.py -v
```

结果：**ASR Python 3.9.6：16/16；ASR Python 3.13.14：16/16；读取回归：29/29**。日志分别 `/tmp/bilibili-task2-final-python39.log`、`/tmp/bilibili-task2-final-python313.log`、`/tmp/bilibili-task2-final-adapter.log`。ASR 测试约 8–10 秒，包含真实 1 秒执行超时和 1 秒受理超时。

用既有临时 jsonschema 依赖对 plugin/tool/skill 应用仓库 Draft202012Validator：**3/3 通过**。已读 PluginCatalog 的 dependency 检查，两组同 Tool 的 required/optional operations 能分别表达。`git diff --check` 通过。

## 行为覆盖

| 场景 | 测试 |
| --- | --- |
| 禁用、缺解释器/模型/依赖，不启动 | test_disabled_missing_python_model_dependencies_do_not_start；test_partial_model_and_overlong_video_are_rejected；test_real_dependency_import_failure_has_no_job |
| 完整受理不超过普通请求期限 | test_duration_lookup_obeys_whole_request_deadline |
| accepted 真回执、运行去重、完成复用、长文本分页 | test_accepted_receipt_deduplicates_and_reads_timed_pages |
| 同时受理仅一个任务 | test_concurrent_acceptance_starts_one_job |
| no_speech 是正常完成 | test_empty_speech_completes_with_no_speech |
| 下载失败、推理失败、超时有终态，音频清理不误删其他文件 | test_download_engine_failure_and_timeout_clean_only_job_audio |
| 下载错分 P / 时长变化不推理 | test_wrong_download_part_and_changed_duration_are_not_transcribed |
| 切换账号不能发布/查询另一账号结果 | test_account_change_prevents_publication_or_cross_account_status；test_publication_uses_same_account_for_check_and_cache_identity |
| 路径穿越、未知回执、超期无终态 | test_invalid_unknown_and_abandoned_job_states |
| 异常受理与损坏记录 | test_launch_failure_has_terminal_state；test_corrupt_job_is_unknown_not_running |
| 虚拟环境入口保留 | test_python_symlink_is_preserved |
| 原调用结束后新 CLI 查询仍可用 | test_cli_query_survives_original_caller |
| 格式 1、操作集合、optional ASR、按需文档 | test_manifest_has_no_platform_writes_and_optional_asr_dependencies；独立 JSON Schema 校验 |

## 真实依赖验证与限制

父级已用仅替换下载器为本地音频复制的方式，跑通真实解释器 + 固定 supervisor/engine + faster-whisper tiny + 状态查询 + read_transcript 全链：约 **1.88 秒**，输出 0–5800ms 英文片段，工作目录清理完成。证据 `/private/tmp/desktop-pet-bilibili-dev-2rnhj2vq/real-worker-smoke.json`；未使用用户账号或播放声音。

父级实际匿名 yt-dlp `extract_info(download=False)` 被平台 HTTP 412 拒绝，因此不声称平台音频下载已经验收；Task 2 单测下载使用 tests/fixtures 中受控依赖。真实登录后字幕/音频与 Windows 行为仍需交付验收步骤，不增加绕过限制或自动重试。

无业务契约偏差。解释器 symlink 与缓存 expected_account 属于既定虚拟环境/账号隔离语义的实现细化，父级已知晓，解释器要求已回填设计。

## Task 3 需要的接口

- `adapter.run` 接受最终七操作；ASR helper 通过此入口完成参数和设置规范化。只有 start_transcription 的宿主 stateMap 增加 accepted，并同时配置 receiptPointer=/receipt、trackingPointer=/tracking；其余操作只映射 completed/failed。父级 CR 核实 runtime 会要求任何声明 accepted 的操作都有回执映射，已相应纠正 USAGE。
- `start_transcription` 使用 `asrPython` 的真实 venv 入口；setup/configure 不可对解释器做 Path.resolve。
- 完整本地模型预检需要 model.bin、config.json、tokenizer.json。requirements-asr.txt 显式安装 faster-whisper、yt-dlp、socksio；普通调用不安装/下载模型。
- `tests/fixtures/bilibili_asr_engine.py:install(directory, **behavior)` 可在受控环境建立 yt_dlp/faster_whisper 两个模块替身，用 PYTHONPATH 传给实际固定 worker，无生产 testMode 或任意程序参数。
- `save_transcript(..., expected_account=None)` 向后兼容；worker 使用受理时 account 指纹，原有 helper 其余签名不变。
