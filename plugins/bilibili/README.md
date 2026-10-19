# B 站内容阅读插件

给主体增加搜索视频、读取简介/评论、分段阅读带时间戳字幕的能力，可选在本地把语音转成文字。通过后台接口工作，不打开播放器或占用鼠标键盘。文字材料不包含画面分析。

主体仍使用已有自主活动安排：有兴趣、预算和授权时选择本 Skill，决定看什么、是否继续和何时结束，也可以等待或做别的事。本包没有固定刷站时间、条数或强制报告，配置不会打开角色的全局自主开关。

- Plugin：`desktop-pet.bilibili`；Tool：`desktop-pet.bilibili.tools`；Skill：`desktop-pet.bilibili.skill`，版本均为 `1.0.0`。
- 操作和结果含义：[USAGE.md](./USAGE.md)；主体阅读方法：[SKILL.md](./SKILL.md)；说明按需加载。
- 接入契约：[插件运行时接入](../../contracts/tools/插件运行时接入.md)；来源和 MIT 许可：[THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md)。
- 无 DSH 依赖，无新主体调度器，无记忆表变更，无平台发评论、私信或点赞操作。

## 准备环境

需要 **Python 3.10+**。长期保留整个插件文件夹；配置保存绝对路径，移动包或 venv 后重新运行 configure。选择包外状态目录，建议每个角色一个，保存依赖、凭证、缓存和任务。不要选插件源码目录或角色记忆数据库。

macOS 在仓库根目录执行（`python3` 需为 3.10+）：

```sh
biliPackage="$PWD/plugins/bilibili"
biliState="$HOME/Library/Application Support/DesktopPetPlugins/bilibili/你的角色ID"
python3 -B "$biliPackage/setup.py" --state-dir "$biliState"
biliPython="$biliState/venv/bin/python"
```

Windows 在仓库根目录打开 PowerShell：

```powershell
$biliPackage = Join-Path (Get-Location).Path 'plugins\bilibili'
$petDataDir = Join-Path $env:APPDATA 'Desktop Pet Team\Desktop Pet'
$petEntries = Get-Content -LiteralPath (Join-Path $petDataDir 'pets.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$petEntries | Select-Object name, profileId
$selectedPetId = '替换为上面真实的小写UUID'
$petProfileDir = Join-Path $petDataDir ('profiles\' + $selectedPetId)
$biliState = Join-Path $petDataDir ('plugin-state\bilibili\' + $selectedPetId)
& py -3 -B (Join-Path $biliPackage 'setup.py') --state-dir $biliState
if ($LASTEXITCODE -ne 0) { throw '准备失败，请查看错误后重跑' }
$biliPython = Join-Path $biliState 'venv\Scripts\python.exe'
```

setup 默认只安装 B 站接口和二维码图像依赖，创建/复用自己的 venv，**不下载语音模型**。失败可重跑同一命令，不清空已有状态。普通工具调用不自动安装依赖或下载模型。

## 人工登录

公开搜索、信息或评论可能匿名可用，字幕通常需要登录。由使用者运行登录助手，它不属于模型 Tool：

```sh
"$biliPython" -B "$biliPackage/login.py" --state-dir "$biliState"
```

PowerShell：

```powershell
& $biliPython -B (Join-Path $biliPackage 'login.py') --state-dir $biliState
```

终端显示本地 `login-qr.png` 路径，手动打开，用 B 站手机 App 扫码确认。最多等待 180 秒，过期后重跑。成功写入 `credential.json`，结束时删除二维码；失败保留旧凭证，不输出 Cookie 或扫码 key。POSIX 下状态目录权限为 700、凭证为 600；Windows 遵循当前账户目录 ACL。状态目录不要加入 Git，凭证不要交给模型。

## 预览并授予一个角色

先正常启动过目标角色，再退出该角色。目录末尾必须是已有的小写 UUID。macOS 常见位置是 `~/Library/Application Support/Desktop Pet Team/Desktop Pet/profiles/<profileId>`，以 launcher 实际数据位置为准。不要编造目录或使用角色显示名。

```sh
petProfileDir="角色现有UUID目录的绝对路径"
"$biliPython" -B "$biliPackage/configure.py" --profile-dir "$petProfileDir" --state-dir "$biliState"
"$biliPython" -B "$biliPackage/configure.py" --profile-dir "$petProfileDir" --state-dir "$biliState" --apply
```

PowerShell：

```powershell
$biliConfigArgs = @('-B', (Join-Path $biliPackage 'configure.py'),
    '--profile-dir', $petProfileDir, '--state-dir', $biliState)
$savedBiliEncoding = [Console]::OutputEncoding
try {
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    & $biliPython @biliConfigArgs
    & $biliPython @biliConfigArgs --apply
    if ($LASTEXITCODE -ne 0) { throw '配置失败，请查看输出' }
} finally {
    [Console]::OutputEncoding = $savedBiliEncoding
}
```

默认仅预览；`--apply` 合并 `plugins.json` 并写 `bilibili.settings.json`，保留其他插件和无关字段。备份位于角色的 `bilibili-backups/<时间-ID>`，替换失败回滚已写文件；相同配置重复应用不新增授权或备份。两个文件分别原子替换，不具备断电时跨文件事务，请先退出角色。手动恢复时按备份内 `manifest.json` 还原原文件，原先不存在的文件则删除。

默认 `--autonomous on` 授权本地主人和 `internal_activity` 使用阅读能力。不会授权外部聊天，不改变全局自主开关、记忆权限或预算。`--autonomous off` 只撤销本包内部自主授权，本地主人仍可请求阅读。重新运行 configure 会启用本包并按当前参数重建本包授权；之后重启角色加载。

## 可选本地语音转写

基础阅读不依赖 ASR。显式准备才会安装 faster-whisper、yt-dlp、模型下载的 SOCKS 代理支持并下载选定模型，支持 tiny/base/small/medium：

```sh
python3 -B "$biliPackage/setup.py" --state-dir "$biliState" --asr-model tiny
"$biliPython" -B "$biliPackage/configure.py" --profile-dir "$petProfileDir" --state-dir "$biliState" \
  --asr-model-path "$biliState/models/tiny"
"$biliPython" -B "$biliPackage/configure.py" --profile-dir "$petProfileDir" --state-dir "$biliState" \
  --asr-model-path "$biliState/models/tiny" --apply
```

PowerShell：

```powershell
& py -3 -B (Join-Path $biliPackage 'setup.py') --state-dir $biliState --asr-model tiny
if ($LASTEXITCODE -ne 0) { throw '转写准备失败，可检查网络后重跑' }
$biliModel = Join-Path $biliState 'models\tiny'
& $biliPython @biliConfigArgs --asr-model-path $biliModel
& $biliPython @biliConfigArgs --asr-model-path $biliModel --apply
```

显式传模型路径才启用 ASR 并授权 `start_transcription`。以后 configure 若省略该参数，会关闭 ASR 并撤销启动授权；已有模型、任务和字幕保留。使用者可在 `bilibili.settings.json` 调整请求超时、最长视频时长和任务期限，后续 configure 保留这些值。

转写固定使用 CPU/int8 和本地模型。tiny 适合检查环境，中文准确率需实测；更大模型增加下载、内存和耗时。默认最长分 P 为 1800 秒，后台期限为 900 秒；语音解码使用 PyAV，无需额外安装系统 FFmpeg。模型缓存也在状态目录。

`accepted` 仅表示受理，主体按原活动节奏查询真实 jobId，完成后读 `source=local_asr`，不紧循环轮询。暂停主体会阻止新调用；已受理任务可以继续到完成或自己的期限，不会自行唤醒主体或发报告。

## 本机验收

重启角色后，确认能发现本 Skill/Tool，再按需加载。独立适配器诊断可执行：

```sh
printf '%s' '{}' | "$biliPython" -B "$biliPackage/adapter.py" --settings "$petProfileDir/bilibili.settings.json" --operation status
```

PowerShell 5.1 的 UTF-8 管道：

```powershell
$savedBiliInput = $OutputEncoding
$savedBiliOutput = [Console]::OutputEncoding
try {
    $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    '{}' | & $biliPython -B (Join-Path $biliPackage 'adapter.py') --settings (Join-Path $petProfileDir 'bilibili.settings.json') --operation status
} finally {
    $OutputEncoding = $savedBiliInput
    [Console]::OutputEncoding = $savedBiliOutput
}
```

退出码 0 表示协议正常，仍需查看 `state/data`。status 的凭证/模型字段只说明文件/目录存在，不证明账号有效或引擎就绪，实际调用继续检查。

| 检查 | 预期 |
| --- | --- |
| 搜索感兴趣主题 | 返回真实 BV/链接，只加载所需说明 |
| 阅读多 P 视频第二 P | part=2 与正确 CID，字幕有时间戳 |
| 用 nextCursor 续读长字幕 | 中文/emoji 不丢，没有全结果被宿主裁剪 |
| 无字幕/未登录/限流 | 明确 no_subtitle/AUTH_REQUIRED/RATE_LIMITED，不编造正文 |
| 已准备 ASR | accepted→查询完成→local_asr；失败保留原因 |
| 主体空闲 | 自己选择阅读或等待，不要求一空闲就访问 |
| 暂停/全局关闭自主 | 不开始新自主读取；已受理任务按上述规则收尾 |
| configure --autonomous off | 自主候选不包含本包，本地主人仍可请求阅读 |

## 已验证与限制

macOS 的真实 PluginRuntime/命令子进程配合受控后端验证发现加载、七操作定义、字幕分页及输出限制、ASR 回执、权限隔离。实际 AIBrain 的受控模型验证自主选择阅读、wait 和全局关闭自主。所有测试角色、材料和凭证均在临时目录，未修改用户实际账号或角色。

2026-10-19 实际匿名搜索、视频信息和评论成功，字幕明确要求登录；匿名 yt-dlp 页面探测遇到 HTTP 412。真实账号字幕、平台音频下载和 Windows 实机还需按上表验收，测试替身不代表真实平台通过。

本机真实 tiny 模型以 CPU/int8、local_files_only=True 转写 5.8 秒合成英文音频，单引擎约 1.15 秒。固定后台链仅替换下载器为本地音频，保留真实引擎，accepted→完成→字幕约 1.88 秒。这不代表长中文视频的速度或识别准确率。

自动化测试兼容 Python 3.9，无需登录或下载模型：

```sh
python3 -B tests/test_bilibili_adapter.py
python3 -B tests/test_bilibili_transcription.py
python3 -B tests/test_bilibili_setup.py
python3 -B tests/test_bilibili_configure.py
QT_QPA_PLATFORM=offscreen ctest --test-dir build -R 'Bilibili|AutonomousChatTests' --output-on-failure
```

Qt 测试需先配置和构建项目，找到 Python 时注册。替身只在 `tests/fixtures`；生产业务输入不提供 testMode、任意 URL 或额外执行程序。
