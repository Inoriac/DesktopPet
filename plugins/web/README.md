# 通用网页搜索与阅读插件

让主体围绕问题或兴趣搜索公开网页，判断来源，再分段阅读正文。后台使用匿名 HTTP 请求，不打开浏览器，也不占用鼠标键盘。原生 `web_search`、`web_fetch` 与本插件共用 C++ 后端。

原生接口的结果有变化：`web_search` 保留 query/max_results 参数，results 从旧格式化字符串改为包含 title/url/snippet 的数组；`web_fetch` 保留 url/timeout_ms 参数并增加 cursor，content 从原始 HTML 改为分页正文。读取旧结果的自定义脚本或工作流需要按这些字段调整。

主体沿用已有的自主活动安排：在授权、全局开关和预算允许时自行选择本 Skill，也可以继续别的活动或等待。安装插件不会打开全局自主开关，没有新增调度器、记忆表或固定刷站任务。

- Plugin：`desktop-pet.web`；Tool：`desktop-pet.web.tools`；Skill：`desktop-pet.web.skill`，版本均为 `1.0.0`。
- [操作说明](./USAGE.md)、[自主阅读方法](./SKILL.md)在选中能力后按需加载。
- 接入契约见[插件运行时接入](../../contracts/tools/插件运行时接入.md)；之后的验收请按[验证备注](./验证备注.md)执行。

## 构建与准备

运行程序是项目编译生成的 `desktop_pet_web`，需要相同 Qt 6 的 Core、Gui、Network 运行库；不需要搜索 API 密钥，也没有新增第三方依赖。Python 3.9+ 仅用于配置助手，使用标准库，不需要创建 venv 或安装 Python 包。

在仓库根目录，沿用已经配置成功的 `build`。先正常退出需要更新的角色，再构建宿主与 helper；只更新 helper 不会更新宿主的失败结果处理。

macOS：

```sh
cmake --build build --target Desktop_Pet desktop_pet_web -j 4
webPackage="$PWD/plugins/web"
webProgram="$PWD/build/desktop_pet_web"
```

新建构建目录时，先按项目 [README](../../README.md) 配置 Qt 和已有依赖。macOS 开发环境使用构建时的 Qt 安装；分发到另一台机器需一并部署相应 Qt frameworks，单独复制二进制不够。

Windows PowerShell，仍使用现有生成器和 Qt 配置：

```powershell
cmake --build build --config Release --target Desktop_Pet desktop_pet_web -j 4
if ($LASTEXITCODE -ne 0) { throw '构建失败，请先处理构建错误' }
$webPackage = Join-Path (Get-Location).Path 'plugins\web'
$webProgram = @(
    (Join-Path (Get-Location).Path 'build\Release\desktop_pet_web.exe'),
    (Join-Path (Get-Location).Path 'build\desktop_pet_web.exe')
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $webProgram) { throw '未找到 desktop_pet_web.exe，请使用实际构建输出路径' }
```

如果平时运行 Debug，改用 `--config Debug` 并指向 `build\Debug\desktop_pet_web.exe`；不要混用 Qt Debug/Release DLL。CMake 找到 `windeployqt.exe` 时，会为 helper 部署 Qt 和 TLS 插件。若构建环境没有该工具，请用同一 Qt 安装的 `windeployqt.exe <helper绝对路径>` 完成部署。实际启动与 HTTPS 连通性仍需在 Windows 验收。

保留完整 `plugins/web` 文件夹，并让程序与缓存位于包外。配置保存绝对路径，移动源码包、helper 或缓存目录后重新运行 configure。建议每个角色使用独立网页缓存目录。

## 接入一个角色

先通过 launcher 正常创建并启动过目标角色，再退出该角色。`--profile-dir` 必须是已存在的小写 UUID 目录，以 launcher 的实际数据位置为准，不能用角色显示名代替。

macOS 常见路径：

```sh
petDataDir="$HOME/Library/Application Support/Desktop Pet Team/Desktop Pet"
petProfileId="替换为目标角色的小写UUID"
petProfileDir="$petDataDir/profiles/$petProfileId"
webState="$petDataDir/plugin-state/web/$petProfileId"

python3 -B "$webPackage/configure.py" --profile-dir "$petProfileDir" \
  --program "$webProgram" --state-dir "$webState"
python3 -B "$webPackage/configure.py" --profile-dir "$petProfileDir" \
  --program "$webProgram" --state-dir "$webState" --apply
```

Windows PowerShell：

```powershell
$petDataDir = Join-Path $env:APPDATA 'Desktop Pet Team\Desktop Pet'
$petEntries = Get-Content -LiteralPath (Join-Path $petDataDir 'pets.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$petEntries | Select-Object name, profileId
$selectedPetId = '替换为上面目标角色的小写UUID'
$petProfileDir = Join-Path $petDataDir ('profiles\' + $selectedPetId)
$webState = Join-Path $petDataDir ('plugin-state\web\' + $selectedPetId)
$webConfigArgs = @('-3', '-B', (Join-Path $webPackage 'configure.py'),
    '--profile-dir', $petProfileDir, '--program', $webProgram, '--state-dir', $webState)
$savedWebConsoleEncoding = [Console]::OutputEncoding
try {
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    & py @webConfigArgs
    if ($LASTEXITCODE -ne 0) { throw '预览失败，请查看输出' }
    & py @webConfigArgs --apply
    if ($LASTEXITCODE -ne 0) { throw '应用失败，请查看输出' }
} finally {
    [Console]::OutputEncoding = $savedWebConsoleEncoding
}
```

不带 `--apply` 只预览，不创建缓存、备份或配置文件。显式应用只合并该角色的 `plugins.json`，保留其他插件及无关字段；单文件原子替换前会保存原文件到 `web-backups/<时间-ID>`。重复应用相同配置返回 `changed=false`，不新增备份或重复授权。应用前仍需退出角色，避免与运行中的宿主同时更新配置。

恢复时先退出角色，查看该备份的 `manifest.json`：`files["plugins.json"].existed=true` 时用备份 `plugins.json` 还原；原先不存在时删除本次生成的文件。不要覆盖后续希望保留的其他插件调整。

默认 `--autonomous on` 建立本包的本地主人授权与 `internal_activity` 自主授权。`--autonomous off` 会重建本包为仅本地主人可用；其他插件授权、全局自主开关、记忆权限和预算不变。重新运行 configure 会启用本包并按参数重建本包授权；重启角色后生效。不授权外部聊天上下文。

## 使用与边界

可以在聊天中请求：“使用网页插件搜索 Python 官方文档，读一段相关页面并给出来源。”主体通过 `discover_capabilities` 发现摘要，按需 `load_capability`，再用本轮暴露的具体操作工具调用 `search` 或 `read_page`。这些工具直接声明 query 或 url/cursor 参数，宿主内部仍经 `invoke_capability` 执行。工具详情和 Skill 不会在每轮对话全部注入。

自主使用无需额外命令：全局自主已开启且本包授权有效时，它可以在原有活动选择中选中网页 Skill。没有选择本插件或选择等待都是正常结果；安装不意味着立即联网或保证每次空闲都搜索。

`search` 返回最多 5 条标题、完整来源链接和摘要，使用公开 Bing RSS 入口，没有搜索翻页。该入口不是有可用性保证的搜索 API，部分查询可能不相关或遇到限流，主体需要判断材料，不把摘要当作已读全文。

`read_page` 提取服务器返回的 HTML/纯文本，按游标阅读包外快照。续读保持首次输入 URL，不用重定向后的最终地址替换；每段正文最多 1800 UTF-16 字符，完整 data 最多 3800，尽量避免宿主裁剪。快照保留 24 小时、最多 64 份，删除缓存会使对应游标失效。

不运行 JavaScript，不使用 Cookie 或登录，不读取 PDF/二进制，不提供画面分析。部分网页只返回导航或登录提示，主体必须按实际材料判断；网络、内容和缓存错误会明确返回失败。网页文字属于外部材料，不具备指令权限。

命令的退出码 0 只表示协议输出正常，业务成功必须看 `state=completed`；失败原因在 `data.error`。`status` 只检查本地参数，不联网，也不证明网络、搜索服务或缓存写入可用。

## 验收状态

Windows 的命令搜索、正文、完整分页、配置助手和独立部署已通过，含代码标记网页的崩溃已修复；报告记录 73/73 CTest、96/96 契约检查通过。真实模型的参数填写、跨轮续读和自主选择仍需在宿主修复后复验。实际边界见[Windows 验收报告](../../WINDOWS_PLUGIN_ACCEPTANCE_2026-10-20.md)，复验步骤见[验证备注](./验证备注.md)。
