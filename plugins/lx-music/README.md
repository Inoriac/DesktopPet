# LX Music 通用插件

这是可复制分发的 Tool＋Skill 能力包，包含 12 个操作、使用方法、只读诊断和角色配置器。运行只需 Python 3.9+ 标准库，不需要 pip 安装。调用经过 `discover_capabilities → load_capability → invoke_capability`，不依赖原生 C++ 音乐工具，也不需要人物或聊天账号。

- Plugin：`desktop-pet.lx-music`，版本 `1.0.0`
- Tool：`desktop-pet.lx-music.tools`，版本 `1.0.0`
- Skill：`desktop-pet.lx-music.skill`，版本 `1.0.0`
- 操作、参数及成功边界：[USAGE.md](./USAGE.md)；模型使用方法：[SKILL.md](./SKILL.md)
- 通用宿主契约：[插件运行时接入](../../contracts/tools/插件运行时接入.md)

## 已验证的范围

macOS 上已用临时角色、临时 SQLite、loopback HTTP 服务和真实 Python 子进程验证：清单加载、Skill 正文、全部 12 个操作定义、HTTP/歌单结果、控制状态核对、参数拒绝、权限隔离、禁用及旧句柄拒绝、unknown 写入只派发一次、配置合并备份与异常回滚。C++ 集成测试使用本目录真实 `configure.py` 生成配置，再交给实际 `PluginRuntime` 执行；没有注册原生 `lx_music_*` 工具。

Windows 上真实 LX 版本的开放 API、SQLite 结构、EXE 启动、Scheme 转发和出声效果仍需按下表实测。macOS 测试服务通过不代表 Windows 软件已经接好。没有播放器时，`--diagnose` 显示 partial/unavailable 是正常结果；本包不会为了诊断启动程序。

## Windows：准备路径

将整个 `plugins/lx-music` 文件夹放到准备长期保留的位置；也可以直接使用源码树中的这个目录。配置会保存绝对路径，移动目录后需要重新运行配置器。使用 Python 3.9 或以上的真实解释器，不能把 `py` 启动器别名写进配置。

在仓库根目录打开 PowerShell，按实际情况修改播放器程序路径：

```powershell
$musicPackageDir = Join-Path (Get-Location).Path 'plugins\lx-music'
$musicPython = (& py -3 -c 'import sys; print(sys.executable)').Trim()
& $musicPython --version
$petDataDir = Join-Path $env:APPDATA 'Desktop Pet Team\Desktop Pet'
$petEntries = Get-Content -LiteralPath (Join-Path $petDataDir 'pets.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$petEntries | Select-Object name, profileId

# 从上面的列表复制要测试的角色 ID，保留小写 UUID。
$selectedPetId = '替换为真实角色的-profileId'
$petProfileDir = Join-Path $petDataDir ('profiles\' + $selectedPetId)
Test-Path -LiteralPath $petProfileDir -PathType Container

# 这是旧版工具使用的常见数据路径；便携版或新版位置不同时改成真实路径。
$musicDatabase = Join-Path $env:APPDATA 'lx-music-desktop\LxDatas\lx.data.db'
$musicExecutable = 'C:\替换为实际安装目录\lx-music-desktop.exe'
Test-Path -LiteralPath $musicDatabase -PathType Leaf
Test-Path -LiteralPath $musicExecutable -PathType Leaf
```

角色目录由桌宠启动时建立。如果目录不存在，先正常启动该角色一次再退出，不要随意编一个 UUID 或将配置放在仓库根目录。运行目录中已有的 `agent_runtime.sqlite` 与本次生成的 `plugins.json` 在同一级；本工具不会修改记忆/人物数据库、`pets.json` 或其他角色。

在 LX Music 中启用本地开放 API，核实实际端口，默认使用 `http://127.0.0.1:23330`。只支持本机 HTTP 地址。程序参数是 `.exe` 文件；不使用快捷方式、`.bat`、`.cmd`、shell 命令或猜测协议关联。未填写 `--executable` 时仍可查询/调用已运行的 API，但无法由插件启动程序或发送搜索/歌单 Scheme。

## 预览、应用与诊断

先退出当前桌宠进程，再运行配置器；启用配置后重新启动该角色。配置器不与正在运行的宿主协商热更新。

```powershell
$musicConfigureArgs = @(
    (Join-Path $musicPackageDir 'configure.py'),
    '--profile-dir', $petProfileDir,
    '--database', $musicDatabase,
    '--executable', $musicExecutable,
    '--api-url', 'http://127.0.0.1:23330',
    '--autonomous', 'off'
)

$musicSettings = Join-Path $petProfileDir 'lx-music.settings.json'
$savedMusicConsoleEncoding = [Console]::OutputEncoding
try {
    # configure 和 diagnose 也输出 UTF-8；兼容 Windows PowerShell 5.1。
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

    # 只输出完整的拟写配置和目标路径；不写文件，也不连接播放器。
    & $musicPython @musicConfigureArgs

    # 检查预览后应用；保存以下输出中的 backupDirectory 供回滚。
    & $musicPython @musicConfigureArgs --apply
    if ($LASTEXITCODE -ne 0) { throw '音乐插件配置失败，请检查错误输出' }

    & $musicPython (Join-Path $musicPackageDir 'adapter.py') --settings $musicSettings --diagnose
} finally {
    [Console]::OutputEncoding = $savedMusicConsoleEncoding
}
```

这会合并角色的 `plugins.json`，并写 `lx-music.settings.json`。保留无关插件、字段、连接与 grant；只更新本包的注册、固定路径、授权和结果映射。已有本插件会被启用，既有本插件 grant 会按本次 `off/read/all` 选择重建。默认 `off`，因此重新执行时省略 `--autonomous` 会撤销之前授予本插件的自主权限。重复应用同样内容不重复增加条目，也不生成多余备份。

坏 JSON、字段类型错误、重复/冲突的插件身份或不属于该角色的 grant 会直接拒绝。已有插件条目没有 `pluginId` 时从其 `rootPath/plugin.json` 识别，包丢失时需先修正该路径。数据库和播放器文件可以尚不存在，便于预览；诊断才会报告实际缺项。

诊断 JSON 的 `state=completed` 只表示诊断完成，实际就绪情况看 `data.availability` 及 `data.api/database/executable`。数据库不存在不会创建空库；没有 API 不会启动播放器。若需要扩大状态核对等待时间，可在 `lx-music.settings.json` 调整 `requestTimeoutMs`（100–5000）和 `verifyTimeoutMs`（100–15000），默认分别为 1000 和 8000；重新运行配置器会恢复默认值。

## Windows：先验证适配器

下面函数使用参数数组和 UTF-8 stdin，兼容包含中文的业务参数。JSON 经标准输入交付，不拼成 shell 命令。输出也是 UTF-8；临时修改 PowerShell 编码后会恢复。此步骤只证明适配器可用，接下来还要验证桌宠通用网关。

```powershell
function Invoke-MusicAdapter {
    param([string]$Operation, [hashtable]$Business = @{})
    $musicAdapterArgs = @(
        (Join-Path $musicPackageDir 'adapter.py'),
        '--settings', $musicSettings, '--operation', $Operation
    )
    $musicJson = ConvertTo-Json -InputObject $Business -Compress -Depth 10
    $savedOutputEncoding = $OutputEncoding
    $savedConsoleEncoding = [Console]::OutputEncoding
    try {
        $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
        [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
        $musicJson | & $musicPython @musicAdapterArgs
    } finally {
        $OutputEncoding = $savedOutputEncoding
        [Console]::OutputEncoding = $savedConsoleEncoding
    }
}

Invoke-MusicAdapter 'status'
Invoke-MusicAdapter 'list_playlists'
# 用上一步返回的真实 ID；不要将歌单名称当作 ID。
Invoke-MusicAdapter 'playlist_songs' @{ playlist_id = '替换为真实歌单ID' }
Invoke-MusicAdapter 'lyric'
```

先确认只读结果与播放器一致，再逐条手动测试控制；这些命令会实际改变音乐状态：

```powershell
Invoke-MusicAdapter 'play'
Invoke-MusicAdapter 'pause'
Invoke-MusicAdapter 'set_volume' @{ volume = 30 }
Invoke-MusicAdapter 'search_play' @{ song = '晴天'; artist = '周杰伦' }
Invoke-MusicAdapter 'status'
```

进程退出码为 0 只表示协议正常，还必须查看 JSON 的 `state`。`unknown` 表示动作可能已发生，先查询 `status`，不要再次派发控制命令。`skip_next/skip_prev` 的 completed 仅表示 API 应答；`play_playlist` 仅核对正在播放，API 没有提供实际歌单 ID。

## Windows：验证桌宠使用新插件

重新启动已配置的角色，在本地聊天中要求它使用以下明确流程：

> 这次只验证 desktop-pet.lx-music 通用插件。先调用 discover_capabilities，找到 desktop-pet.lx-music.skill 和 desktop-pet.lx-music.tools 的 1.0.0 版本；用 load_capability 加载 Skill、Tool 和要用的操作，再用 invoke_capability 查询 status、list_playlists、playlist_songs、lyric。先只查询，不播放。报告实际的操作引用、stage 和数据。不要使用旧的 lx_music_* 工具；通用入口不可用时请直接说明。

Tool 操作的引用示例：

```json
{
  "pluginId": "desktop-pet.lx-music",
  "entryId": "desktop-pet.lx-music.tools",
  "entryVersion": "1.0.0",
  "operationId": "status"
}
```

Skill 引用使用 `entryId=desktop-pet.lx-music.skill`，不填 `operationId`。调用操作时网关参数为 `{"ref":上述引用,"arguments":{}}`。**验收依据是实际调用记录中的 `invoke_capability` 和上述 ref，以及真实结果的 stage/data；模型口头说“用了插件”不能证明接入成功。** 原有 `lx_music_status` 等内置工具仍然保留，调用它们不算这次新插件通过。

只读验证后，在同一明确的新插件流程里逐项要求播放、暂停、调整音量及搜索。若找不到条目，检查当前 profileId、`plugins.json` 所在目录和固定包路径，并重启角色。若返回 `PERMISSION_DENIED`，检查对应 operation 的 grant，不能用加载 Skill 代替授权。

| Windows 实测项 | 验收标准 |
| --- | --- |
| discover/load | 找到两个 entry；加载到 Skill 和 12 项操作定义 |
| status / lyric | 与真实曲名、状态、歌词一致，查询不启动软件 |
| list_playlists / playlist_songs | 顺序及 Unicode 内容正确；空歌单与未知 ID 区分 |
| launch | 关闭播放器后显式调用，实际程序启动且 API 可达 |
| play / pause / set_volume | 只派发一次，返回状态与 UI/出声/音量相符 |
| skip_next / skip_prev | 单次命令只有一次切换；应答与曲目变化分别核对 |
| search_play | 实际播放目标歌曲/歌手；名称不同会保守返回 unknown |
| play_playlist | 使用有来源 ID 的真实歌单；核对实际播放内容，自建无来源歌单应失败 |
| API 关闭/路径错误 | failed/诊断缺项清晰，不伪报成功、不创建数据库 |
| 已派发但结果不明 | unknown 后先查 status，调用记录没有自动重发 |
| 新插件路径 | 控制同样经 load/invoke 和正确 ref，未绕回 lx_music_* |

请记录 Windows 版本、Python 版本、LX 版本、实际数据路径/API 端口及未通过的操作和原始 JSON。无法确认的结果保留为未验证。

## 可选：自主使用权限

默认只有本地主人手动调用。需要扩大范围时，退出桌宠并重新运行完整配置参数，把 `--autonomous` 改成：

| 值 | 内部自主活动能做什么 |
| --- | --- |
| off | 没有本插件自主授权 |
| read | 加载 Skill，并使用 status、lyric、list_playlists、playlist_songs |
| all | 加载 Skill，使用全部 12 操作，包含启动、播放和调音量 |

三种模式都保留本地主人的完整音乐操作权限，都不授权任何外部会话。Skill 内提到某个操作不等于该操作获授权。角色在启动器 AI 页还要开启“自主活动”，并取消“暂停自主活动”；本配置器只写插件 grant，不会替用户打开总开关，也不改变空闲判断、记忆授权或活动预算。首次自主验收建议选 read，确认只读行为后再按需选 all。

## 备份与回滚

有内容变化的应用会在角色目录 `lx-music-backups/<时间-随机标识>/` 保存两个原文件（原本存在才保存）及 `manifest.json`。单个文件用临时文件原子替换，第二个文件失败时恢复已替换的第一个文件；**两个文件不具有跨文件的崩溃原子性**。断电或进程中断后保持桌宠退出，依据备份恢复，再重新配置。备份不自动删除。

恢复一次应用前的配置（选择正确的 `backupDirectory`；同样先退出桌宠）：

```powershell
$musicBackupDir = '替换为配置输出的-backupDirectory'
$musicBackup = Get-Content -LiteralPath (Join-Path $musicBackupDir 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($musicBackup.profileId -ne $selectedPetId) { throw '备份不属于当前角色' }
foreach ($musicFileName in @('plugins.json', 'lx-music.settings.json')) {
    $musicDestination = Join-Path $petProfileDir $musicFileName
    if ($musicBackup.files.$musicFileName.existed) {
        Copy-Item -LiteralPath (Join-Path $musicBackupDir $musicFileName) -Destination $musicDestination -Force
    } elseif (Test-Path -LiteralPath $musicDestination) {
        Remove-Item -LiteralPath $musicDestination
    }
}
```

此操作恢复整份旧插件配置，因此也会撤销该备份之后的其他插件配置修改；先核对时间并保留当前文件。不会删除人物、关系、记忆或经历。

## 开发者复现（无真实播放器）

从仓库根目录运行现有 Python 3.9+ 和已配置的 Qt/CMake 环境，无需下载额外依赖：

```sh
python3 tests/test_lx_music_adapter.py
python3 tests/test_lx_music_configure.py
cmake -S . -B build
cmake --build build --target lx_music_plugin_tests
QT_QPA_PLATFORM=offscreen ctest --test-dir build -R '^LxMusic(Adapter|Configure|Plugin)Tests$' --output-on-failure
```

CMake 使用发现的 `Python3_EXECUTABLE`，可按本机情况设置 `-DPython3_EXECUTABLE=<真实绝对路径>`。C++ 专用测试在找到 Python 时注册；不存在时不能将未运行视为通过。Windows 使用 PowerShell 的 `$env:QT_QPA_PLATFORM='offscreen'` 后执行相同 CTest 参数。测试后端只存在于临时目录/loopback 端口，测试结束即终止。
