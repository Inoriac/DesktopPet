# QQ 会话插件

`desktop-pet.qq` 通过 OneBot v11 正向 WebSocket 对接 NapCat / SnowLuma，把 QQ 私聊和群聊接入 DesktopPet 现有工具、观察、人物关系与活动运行时。不控制鼠标或屏幕，也不启动另一个模型或 bot 回复循环。

提供 `status / list / read / send / receipt`、一个按需加载的 Skill，以及默认关闭的消息 source。主体自行决定浏览、等待或回应。**内部活动可以自主浏览；发送必须发生在对应外部会话活动中。当前没有从内部活动任意选人主动开聊或切换会话的工具。** 外部会话机会由现有宿主机制提供，不能通过插件伪造上下文。

首版支持文本、回复引用、私聊/群聊隔离、近期历史查询、监听与重放、可追踪发送回执。收到图片/语音/视频等会显示占位，尚不理解或发送媒体。离线消息能否补齐取决于后端近期历史接口，不能保证完整补齐。

实现经过本地假 OneBot 服务和宿主网关验证。按本次交付要求，**没有登录、连接或发送真实 QQ 消息**；实际 NapCat/SnowLuma 及 Windows 验收交给后续人员，见[验收交接](验收交接.md)。不能把受控测试通过理解为真实平台已验收。

## 文件与契约

| 文件 | 作用 |
| --- | --- |
| plugin.json / tool.json / skill.json / source.json | 插件、五个工具操作、方法和观察源声明 |
| [USAGE.md](USAGE.md) / [SKILL.md](SKILL.md) | 选中能力后才加载的工具说明和活动方法 |
| adapter.py / transport.py / common.py | 短命令、WebSocket、消息转换与设置校验 |
| source.py / journal.py / delivery.py | 长连接监听、包外 SQLite、发送与查询回执 |
| setup.py / configure.py | 包外安装、单角色配置预览和原子应用 |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | API 来源、研究提交、依赖与授权说明 |

公共接入契约见[观察源与聊天插件接入](../../contracts/tools/观察源与聊天插件接入.md)和[插件运行时接入](../../contracts/tools/插件运行时接入.md)。插件不直接读写宿主数据库，也不添加 QQ 专属核心分支。

## 安装与连接设置

1. 安装 Python 3.9+。由使用者准备已登录目标 QQ 账号的 NapCat 或 SnowLuma，并开启 **OneBot 正向 WebSocket 服务端**；本插件是连接该服务的客户端，不是反向 WS 接收端。记录后端、QQ 客户端与协议配置版本。
2. 给该角色分配独立的包外 stateDir。setup 只创建 venv 并安装 `websockets==15.0.1`，不安装 MaiBot、QQ 客户端或 OneBot 后端。
3. 创建包外 UTF-8 **无 BOM** 的 settings.json；需要鉴权时将非空的可打印 ASCII token 单独写入包外文本文件。插件通过 `Authorization: Bearer ...` 发送 token，wsUrl 中不要加用户名、密码或 `access_token` 查询参数。

macOS / Linux，在仓库根目录执行（替换账号、会话与路径）：

```sh
qq_state="$HOME/Library/Application Support/DesktopPet/qq/角色UUID"
python3 -B plugins/qq/setup.py --state-dir "$qq_state"
qq_python="$qq_state/venv/bin/python"
```

Windows PowerShell，在仓库根目录执行：

```powershell
$qqState = Join-Path $env:LOCALAPPDATA 'DesktopPet\qq\角色UUID'
$pyLauncher = Get-Command py.exe -ErrorAction SilentlyContinue
if ($pyLauncher) {
  & $pyLauncher.Source -3 -B plugins/qq/setup.py --state-dir $qqState
} else {
  $basePython = (Get-Command python.exe -ErrorAction Stop).Source
  & $basePython -B plugins/qq/setup.py --state-dir $qqState
}
if ($LASTEXITCODE -ne 0) { throw 'QQ 插件环境创建失败，请查看上方输出' }
$qqPython = Join-Path $qqState 'venv\Scripts\python.exe'
$qqSettings = Join-Path $qqState 'settings.json'
$qqToken = Join-Path $qqState 'token.txt'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$settings = @{
  backend = 'napcat'
  wsUrl = 'ws://127.0.0.1:3001'
  selfId = '999999999'
  connectionKey = 'pet-qq-account'
  stateDir = $qqState
  tokenFile = $qqToken
  allowedConversations = @('group:123456789', 'private:234567890')
  requestTimeoutSeconds = 10
}
[System.IO.File]::WriteAllText($qqSettings, ($settings | ConvertTo-Json -Depth 5), $utf8NoBom)
# 使用编辑器将后端配置的 token 写到 token.txt，保存为 UTF-8 无 BOM。
# 后端明确不使用 token 时，从 settings 删除 tokenFile 后重新写入。
```

PowerShell 5 的 `Set-Content -Encoding UTF8` 会带 BOM，请使用上述写法。设置文件示例（macOS，路径需替换成绝对路径）：

```json
{
  "backend": "napcat",
  "wsUrl": "ws://127.0.0.1:3001",
  "selfId": "999999999",
  "connectionKey": "pet-qq-account",
  "stateDir": "/Users/you/Library/Application Support/DesktopPet/qq/角色UUID",
  "tokenFile": "/Users/you/Library/Application Support/DesktopPet/qq/角色UUID/token.txt",
  "allowedConversations": ["group:123456789", "private:234567890"],
  "requestTimeoutSeconds": 10
}
```

`backend` 只能是 napcat 或 snowluma。selfId 必须等于该连接实际登录的 QQ 号，每次连接先核对它；错误账号不会继续查询或发消息。connectionKey 是稳定连接身份命名空间，不使用昵称。账号改变时使用新的 connectionKey 和全新 stateDir，不复用旧日志。会话键的 `group:` / `private:` 前缀必不可少；私聊填写对方 QQ 号。allowedConversations 不会自动扩张。

## 接入角色

先停止该角色桌宠，找到 launcher 创建的、名称为小写 UUID 的实际角色目录。保持源码包、settings、token、venv、journal 分开：所有可变文件都放包外。configure 只查看设置结构，不读 token、不联网，也不创建日志。

macOS / Linux：

```sh
"$qq_python" -B plugins/qq/configure.py --profile-dir '/实际角色目录/角色UUID' --settings "$qq_state/settings.json"
"$qq_python" -B plugins/qq/configure.py --profile-dir '/实际角色目录/角色UUID' --settings "$qq_state/settings.json" --source on --apply
```

Windows PowerShell：

```powershell
& $qqPython -B plugins/qq/configure.py --profile-dir 'C:\实际角色目录\角色UUID' --settings $qqSettings
& $qqPython -B plugins/qq/configure.py --profile-dir 'C:\实际角色目录\角色UUID' --settings $qqSettings --source on --apply
```

不带 `--apply` 只输出可审阅预览；应用时仅修改该角色 plugins.json，在角色目录 qq-backups 下保留原文件。重复应用相同配置不再写入；其他插件和自定义设置保留。配置或允许会话变更后重新运行 configure，并重启角色载入一致的绑定。

`--source on|off` 控制后台接收，默认 off。`--autonomous on|off` 控制本插件的自主工具授权，默认 on，**不修改宿主自主活动总开关**。可以 source on + autonomous off，仅接收归档而不授自主调用。即使 source off，显式 read 仍可查询近期历史。

本地主人/内部活动授予 status/list/read/receipt 与 Skill，绝不授 send。外部会话授予精确 chat_id 的 read/send/receipt，不授 list，防止一个群看到另一个群的摘要。人物对应的是 `qq + connectionKey + QQ号`；不会把任何 QQ 用户自动认作主人。

## 运行边界与维护

- source 长连接与 command 短连接共享包外 SQLite 日志。游标包含日志实例 ID；重启从宿主已提交游标重放，查询不会推进监听水位。后端断线期间仍可能有消息缺口。
- list 和 read 都分页；read 按体积提前结束页面，用 next_cursor 继续。单条长正文可能在模型输出中被宿主截短，原始正文（最多 8000 UTF-16）仍归档；本版不提供消息正文内部翻页。
- 发送只派发一次，核对远端 ID、目标、发送者、实际正文与时间后才 reported completed。SnowLuma 群发送还核对正整数 message_seq。缺失证据返回 unknown，不自动重发。
- receipt 只核对旧发送；没有 remote ID 的未知凭证无法自动恢复确认。后续确认不会改写宿主旧 unknown 派发记录；新消息经 read/source 归档。
- 数据库保存消息正文与发送凭证，目前不自动裁剪。自行保管 stateDir 并留意磁盘增长；数据库绑定账号身份，不删除数据库来“修复”游标错误。
- 更新代码后重新加载插件。禁用/移除插件不会删除日志；回滚配置时停止角色，恢复 qq-backups 中 plugins.json。首次配置原来不存在该文件时按备份 manifest 记录移除它；不要覆盖之后添加的其他插件修改。
- 第三方 WebSocket 服务和凭据属于部署配置。本插件无自动安装、扫码登录、截图、群管理、文件发送、媒体理解和平台权限提升操作。

## 本地受控回归

只使用 tests/fixtures/qq_backend.py 假服务和临时数据，不需要 QQ 账号：

```sh
"$qq_python" -B tests/test_qq_adapter.py
"$qq_python" -B tests/test_qq_delivery.py
"$qq_python" -B tests/test_qq_configure.py
cmake -S . -B build -DQQ_TEST_PYTHON="$qq_python"
cmake --build build --target qq_plugin_tests
ctest --test-dir build -R '^QQ' --output-on-failure
```

Windows 用 `& $qqPython` 执行 Python；多配置生成器构建加 `--config Debug`，CTest 加 `-C Debug`。Qt 查找路径沿用项目构建环境。QQ_TEST_PYTHON 只选择 QQ 测试解释器，不替换项目全局 Python；没装 websockets 时适配器、发送和 C++ 集成测试明确 skip，配置测试仍可运行。实际平台验收请使用[验收交接](验收交接.md)，不要将假服务命令指向私人账号。

Windows 运行 `QQPluginTests` 前应把匹配的 Qt 和编译器 `bin` 放进当前终端的 `PATH`；缺少运行时 DLL 时常见退出码为 `0xc0000135`。如果 setup 因用户配置的 pip 镜像提示找不到 `websockets==15.0.1`，可只为本次终端临时指定官方索引后重跑，已有 stateDir 会保留：

```powershell
$env:PIP_INDEX_URL = 'https://pypi.org/simple'
# 重新执行上面的 setup 命令；成功后可关闭终端或恢复原环境变量。
```
