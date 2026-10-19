# Task 3：安装、角色配置与自主接入验证

父级 CR：已逐项阅读三个安装配置入口、真实命令替身、PluginRuntime/AIBrain 测试、CMake 和 README；修复 Windows 登录输出编码，保留实际 venv 入口。真实临时角色配置烟测和最终 71/71 CTest 组全部通过，无阻断问题。按持续有效授权提交。

关联设计：§1、§3.4 全 7 子节，依赖 §3.2/§3.3。日期：2026-10-19。实现已交父级 CR，未 commit、未修改计划进度；未使用用户账号、修改真实角色或联网写平台。build/ 已释放给父级做最终全量验证。

## 实现与 sanity check

- 已读取设计、impl-plan、context、Task 1/2 记录及 evoloop-code-impl；核实已有 `plugins/lx-music/configure.py` 的 prepare/apply、`PluginRuntime::executeGateway/loadCapability`、`AgentBootstrap::start`、AIBrain 与 tests 的 Client/roles/choice/plan/tool/verify。保持原测试目录/CMake 组织，无主系统 C++ 生产代码改动。
- `setup.py` 创建或复用 stateDir/venv，仅安装基础依赖；`--asr-model` 才安装 ASR 包并下载选定模型。pip 禁用缓存，HF_HOME 指向状态目录；不清空原 venv，失败可重跑。保留 sys.executable 的虚拟环境入口符号链接。
- `login.py` 使用实际库的异步 generate_qrcode/check_state、同步 get_qrcode_picture().content/get_credential，明确选取六个 credential 字段。私有原子凭证文件、180 秒总期限、二维码退出清理；超时/失败不覆盖旧凭证，错误不回显 key/Cookie。二维码路径只供使用者手动打开，登录未成为模型 Tool。
- `configure.py` 独立实现，不导入音乐插件。默认预览、--apply 合并/备份/回滚/幂等；仅写同一角色 plugins.json 与 bilibili.settings.json，保留无关插件/字段及本包请求/任务期限设置。默认 on 授予 local_owner/internal_activity，不改变全局自主、记忆权限和预算；off 仅撤销本包自主授权。显式模型路径才开启 ASR/start 写授权。
- 每操作配置 /state、/data；仅 start_transcription 配 accepted、/receipt、/tracking。ASR 关闭时基本 Skill 正常可加载。实际 Tool 清单列出全部七项操作，权限在具体调用时核对；不要求宿主隐藏未授权操作说明。
- 所有新入口先禁止字节码写入，命令使用 -B；login/setup stdout/stderr 显式 UTF-8，configure 使用 UTF-8 bytes；测试覆盖 cp1252 控制台。父级提示后将 Task 2 JSON/源码文件读写明确为 UTF-8，symlink 测试在 Windows 无权限时只跳过该用例。
- `tests/fixtures/bilibili_backend.py` 运行实际 configure CLI 生成临时角色，只有测试 fixture 把绑定脚本替换为受控后端；真实 adapter/cache/worker 和宿主网关保留。ASR 使用前序 tests-only 依赖替身。生产没有 testMode 或任意 URL/程序业务参数。
- README 给出 macOS/PowerShell 安装、扫码、单角色预览应用、ASR 开关、备份恢复、UTF-8 协议诊断、自主/暂停语义与实机验收边界，契约索引和当前适配验证增加入口。

## RED → GREEN

1. 先写 Setup/Login 和 Configure 测试：第一次运行分别报 `ImportError: cannot import name 'setup'` / `cannot import name 'configure'`。实现三个 CLI 后最初 17/17 通过；新增 cp1252 登录 CLI 回归后为 18/18。
2. 先写真实 PluginRuntime 网关测试并注册 CMake，构建成功后首次运行三项均因缺少 `tests/fixtures/bilibili_backend.py` 而失败；实现 fixture 后 ASR/权限通过，基本读取的一项预期从“清单只列授权六操作”纠正为现有宿主的“清单完整七操作、未授权调用拒绝”，最终三项行为全部通过。
3. AIBrain 首轮 wait 与全局关闭通过，选择 Skill 未进入任务。定位为原测试 helper `choice()` 使用 CapabilityRef::toJson，会省略空 operationId，而模型选择 responseSchema 要求显式空字符串。只修正测试 helper 输出为 `operationId=""`，使其满足真实模型响应契约；后续三组全部通过，无主系统修复或设计偏移。

## 验证结果

```sh
python3 -B tests/test_bilibili_setup.py
python3 -B tests/test_bilibili_configure.py
/Users/huang/.qt-build-env/bin/cmake -S . -B build
/Users/huang/.qt-build-env/bin/cmake --build build --target bilibili_plugin_tests -j 4
/Users/huang/.qt-build-env/bin/cmake --build build --target autonomous_chat_tests -j 4
QT_QPA_PLATFORM=offscreen /Users/huang/.qt-build-env/bin/ctest --test-dir build -R '^Bilibili' --output-on-failure
QT_QPA_PLATFORM=offscreen DESKTOP_PET_TEST_PYTHON=/Users/huang/Desktop/DesktopPet/launcher/.venv/bin/python build/autonomous_chat_tests bilibili_existing_autonomy_selects_reads_or_waits
git diff --check
```

- Setup/Login：**9/9**；Configure：**9/9**，Python 3.9.6。
- Bilibili CTest **5/5** 组通过，约 15.4 秒（该次 Setup 为增加最后 UTF-8 用例前的 8 例；增加后独立 9/9 通过）。包含读取 29、ASR 16、安装/登录与配置、真实网关三项行为。
- AIBrain 新增三组 **3/3** 通过，约 2.6 秒。真实候选出现 Skill 摘要且没有正文；受控模型选择后实际执行 search 和第二 P 字幕读取、落字幕快照；wait 未触发任何后端请求；全局关闭未触发选择或请求。
- 网关实际 payload 经过 ToolResultSanitizer，逐页重组 `字幕🙂` × 900，无损、时间戳保持 1250–5000ms、完整 payload <=6000。续页只读缓存，真实后端字幕请求一次。
- ASR 真实固定后台进程经宿主 accepted/receipt/tracking → transcription_status completed → local_asr，返回真实 jobId 与时间段；无模型授权 start 调用拒绝但 Skill 可加载。
- 内部 off、外部会话、已加载后禁用均在执行前拒绝，后端日志为零。包内无 __pycache__。git diff --check 通过。

父级另已用真实开发 venv 在临时 UUID 角色运行 configure 预览→apply→第二次 apply→按生成配置调用 status：预览不写文件、应用幂等、保留 venv 入口、依赖 ready 且显式 ASR 模型可见，全部通过。没有再次安装开发依赖或下载模型。

## 设计行为覆盖

| 场景 | 测试 |
| --- | --- |
| 默认基础安装、显式模型安装、不破坏原环境、失败可重跑 | SetupTests 五项 |
| QR 成功、过期保留旧凭证、网络超时、无凭证回显、Windows 编码 | LoginTests 四项 |
| 预览/合并/幂等、on/off、ASR 授权与结果映射、备份/替换回滚 | ConfigureTests 九项 |
| 分页读取、UTF-8、实际宿主输出限制、ASR 可选 | discovery_optional_asr_and_paginated_real_command_output |
| 内部关闭、外部上下文、禁用拦截 | internal_off_external_and_disabled_are_rejected_before_execution |
| 七操作定义、实际后台回执/查询/字幕 | asr_receipt_status_and_source_through_real_gateway |
| 实际自主候选与使用、等待和全局关闭 | bilibili_existing_autonomy_selects_reads_or_waits 三组数据 |

## 偏差与边界

没有业务契约或系统设计偏差。计划中的“现有工具契约适配验证.md”位于 contracts/tools/，按真实路径修改。测试 helper 必填空 operationId 和清单完整操作属于既有宿主语义的测试修正。

QR 全流程使用受控登录对象，没有实际扫码。匿名平台只读、真实 tiny 引擎和下载受限结果沿用前序验证：真实账号字幕、平台音频下载及 Windows 实机仍未验收，README 明确区分。全项目构建、全量 CTest、契约检查和最终提交由父级执行。
