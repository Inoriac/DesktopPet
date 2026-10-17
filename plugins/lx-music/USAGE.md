# LX Music 工具使用约定

此包提供 12 项操作，复用 LX Music 的本地开放 API 和只读歌单数据库。
宿主固定 Python、适配器与连接配置路径；调用方只提交操作定义中的业务 JSON。
所有查询都不会启动播放器，人物或外部账号不是使用前提。

| 操作 | 参数 | completed 的含义 |
| --- | --- | --- |
| status | `{}` | 从 `/status` 取得真实 JSON 状态，保存在 `data.status` |
| lyric | `{}` | 从 `/lyric` 取得 UTF-8 LRC 文本或 JSON 中的 lyric，保存在 `data.lyric` |
| list_playlists | `{}` | 只读数据库，按 position 列出歌单，返回 `data.items` |
| playlist_songs | `{"playlist_id":"..."}` | 验证歌单存在，按歌曲顺序只读列出 `data.items`；空歌单返回空数组，未知歌单失败 |
| launch | `{}` | API 已经可用，或者启动指定程序后确认 API 可用 |
| play | `{}` | 单次 `/play` 应答后观察到 `status=playing` |
| pause | `{}` | 单次 `/pause` 应答后观察到 `status=paused` |
| skip_next / skip_prev | `{}` | `/skip-next` 或 `/skip-prev` 明确应答；这不证明歌曲已经切换 |
| set_volume | `{"volume":50}` | volume 必须是 0–100 的整数；单次写入后观察到同样音量 |
| search_play | `{"song":"晴天","artist":"周杰伦"}` | artist 可选。指定程序收到一次编码后的 searchPlay URL，之后观察到正在播放且歌名与可选歌手匹配 |
| play_playlist | `{"playlist_id":"..."}` | 只读取得该歌单的来源及来源 ID，指定程序收到一次歌单 URL，随后观察到正在播放；API 没有证明歌单 ID 匹配 |

`search_play` 的名称核对使用 Unicode 规范化、忽略大小写和首尾空格后的精确匹配。
版本后缀、合作歌手等实际名称不同会返回 unknown；保留实际状态，由用户核对。
只有 `launch`、`search_play`、`play_playlist` 会启动配置中的程序；
HTTP 控制操作要求 API 已运行，不会为重试自动启动或再次派发。

每次返回一个 JSON 对象：`{"state":"completed|failed|unknown","data":{...}}`。
业务错误位于 `data.error.code/message`，协议正常时退出码为 0。
确定的输入/配置/前置条件错误为 failed；写请求派发后断连、超时或目标状态无法核对为 unknown。
unknown 不等于没执行，禁止自动重发；只能先调用 status 核对。调用被宿主超时/取消时同样如此。
切歌是否发生不能通过重复切歌来确认。

宿主需为每项 reported 操作配置 `dataPointer=/data`、`statePointer=/state` 和
`stateMap={"completed":"completed","failed":"failed","unknown":"unknown"}`。
不要设置 `errorPointer=/data/error`，它会把 unknown 误映射成 failed。
总调用超时建议至少 30 秒，以覆盖受限的 API 请求、验证时间和进程启动开销。

歌单查询最多返回 1000 行，并限制输出大小；`data.truncated=true` 明确表示未返回完整列表。
超过 4096 字符的数据库字段会失败，避免无界结果。数据库打不开、结构不匹配或锁定时失败，
缺文件不创建数据库。歌单无 source/sourceListId 时无法通过 Scheme 播放。
来源 ID 中已有的路径分隔符 `/` 保留；空格、查询符、片段符等其他特殊字符按 URL 编码。

## 宿主连接配置

连接 JSON 包含：

```json
{
  "apiUrl": "http://127.0.0.1:23330",
  "database": "<当前系统上的绝对 SQLite 文件路径>",
  "executable": "<可选：当前系统上的绝对播放器程序文件路径>",
  "requestTimeoutMs": 1000,
  "verifyTimeoutMs": 8000
}
```

apiUrl 仅允许 HTTP loopback origin（127.0.0.0/8、::1 或 localhost），无凭据、路径、查询或片段；
不使用代理、不跟随重定向。requestTimeoutMs 范围 100–5000，verifyTimeoutMs 范围 100–15000。
executable 指程序文件；Windows 不接受 `.bat`/`.cmd`；macOS 如需真实使用应选择应用包内程序文件，而非 `.app` 目录。
本包不猜测安装路径，不使用 shell 或系统协议关联回退。

`python adapter.py --settings <绝对连接配置路径> --operation <操作>` 从 stdin 接收 UTF-8 JSON。
`python adapter.py --settings <绝对连接配置路径> --diagnose` 不读取业务输入，只读检查配置、程序、数据库及 API。
诊断正常完成时 state=completed；`data.availability` 的 ready/partial/unavailable 表示依赖是否齐全，
不能将“诊断完成”理解为“播放器已就绪”。程序存在也不能证明 Windows 上实际能启动或播放。

具体安装、角色配置与 Windows 验收见随包 README；安装和 Skill 加载本身不会赋予自主操作权限。
