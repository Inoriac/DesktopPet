---
name: lx-music
description: |
  LX Music 桌面版播放器控制工具。通过本地 HTTP API 控制音乐播放、获取播放状态和歌词，还能直接读取用户歌单列表并播放。
  触发场景：
  - "控制 LX Music 播放"、"暂停"、"上一曲"、"下一曲"
  - "启动 LX Music"、"打开 LX Music"、"自动拉起 LX Music"
  - "LX Music 播放状态"、"现在播放什么歌"
  - "获取当前歌词"、"查看当前歌词"、"LX Music 当前播放的歌词"
  - "调整 LX Music 音量"、"音量调大/调小"、"静音"
  - "LX Music 收藏当前歌曲"、"收藏这首歌"、"取消收藏"
  - "播放一首歌"、"放一首周杰伦的歌"、"搜索并播放"
  - "跳到30秒"、"快进到1分钟"、"跳到指定时间"
  - "播放歌单"、"播放我的收藏歌单"、"播放许嵩的歌单"、"打开歌单"
  - "查看我有哪些歌单"、"列出所有歌单"、"歌单列表"
  - "播放第3首歌"、"从第5首开始播放"
  - 任何涉及 LX Music 播放控制、歌曲搜索、歌单操作的请求
allowed-tools: Bash(curl *), Bash(python *)
---

# LX Music 控制

基于 LX Music 开放 API 的 HTTP 控制接口 + 本地 SQLite 歌单数据库。**所有信息已内嵌，无需查阅在线文档。**

## 前置条件

LX Music 桌面版需开启「开放 API 服务」（设置 → 开放 API → 启用）。默认地址：`http://127.0.0.1:23330`

工具侧已内置自动拉起逻辑：当 `/status`、播放、暂停、切歌、音量等 HTTP API 不可达时，会优先通过 LX Music 桌面版可执行文件静默启动程序，等待开放 API 可用后重试原操作。

不要打开裸 `lxmusic://`：这会被 LX Music 识别为未知地址并弹出 `unknown url`。搜索点歌或歌单播放时，只打开完整业务 URL，例如 `lxmusic://music/searchPlay/...` 或 `lxmusic://songlist/play/...`。

搜索点歌和播放歌单会在拉起 Scheme URL 后轮询 `/status` 验证播放状态：
- `lx_music_search_play` 必须验证状态为 `playing`，且当前歌曲/歌手与请求参数匹配，才算成功。
- `lx_music_play_playlist` 必须验证状态为 `playing`，才算成功。
- 如果无法确认播放，会返回失败，避免误报“已播放”。

歌单查询脚本依赖 Python（读取 `LxDatas/lx.data.db` SQLite 数据库）。

## 快速命令索引

| 操作 | 命令 |
|------|------|
| 启动并等待 API | `lx_music_launch` |
| 播放 / 暂停 | `curl $LX_API_URL/play` / `curl $LX_API_URL/pause` |
| 上一曲 / 下一曲 | `curl $LX_API_URL/skip-prev` / `curl $LX_API_URL/skip-next` |
| 获取状态 | `curl $LX_API_URL/status` |
| 获取歌词 | `curl $LX_API_URL/lyric` |
| 音量控制 | `curl $LX_API_URL/volume?volume=80` |
| 静音 | `curl $LX_API_URL/mute?mute=true` |
| 收藏 | `curl $LX_API_URL/collect` |
| 搜索播放 | `lx_music_search_play`，运行时打开 `lxmusic://music/searchPlay/歌曲名-歌手` |
| 跳转进度 | `curl $LX_API_URL/seek?offset=30` |
| 列出歌单 | `python scripts/playlist.py` |
| 获取歌单播放 URL | `python scripts/playlist.py url <list_id>` |

## 歌单查询

歌单数据存在 LX Music 的 SQLite 数据库中，可直接查询。

### 列出所有歌单

```bash
python scripts/playlist.py
```

返回格式：
```json
[
  {"id": "wy__2244784586", "name": "许嵩经典合集", "source": "wy", "sourceId": "2244784586"},
  {"id": "userlist_1667638754021", "name": "Imagine", "source": null, "sourceId": null},
  ...
]
```

### 查看歌单中的歌曲

```bash
python scripts/playlist.py songs <list_id>
```

### 获取歌单播放 URL

```bash
python scripts/playlist.py url <list_id>
```

**ID 格式说明**：
- 网易云：`wy__数字ID` → `lxmusic://songlist/play/wy/数字ID`
- QQ：`tx__数字ID` → `lxmusic://songlist/play/tx/数字ID`
- 酷狗：`kg__id_数字ID` → `lxmusic://songlist/play/kg/id_数字ID`
- 咪咕：`mg_字符串ID` → `lxmusic://songlist/play/mg/字符串ID`
- 用户自建：`userlist_时间戳` → 不支持 Scheme URL 播放

## API 详情

### 状态查询

```bash
# 获取完整状态
curl "$LX_API_URL/status"

# 过滤字段
curl "$LX_API_URL/status?filter=status,name,singer,progress"
```

响应字段：
- `status` — `playing` | `paused` | `stoped` | `error`
- `name` — 歌曲名
- `singer` — 艺术家
- `albumName` — 专辑名
- `duration` — 总时长（秒）
- `progress` — 当前进度（秒）
- `volume` — 音量 0-100
- `mute` — `true` | `false`
- `collect` — `true` | `false`

### 歌词获取

```bash
# LRC 歌词（纯文本）
curl "$LX_API_URL/lyric"

# 所有歌词类型（JSON）
curl "$LX_API_URL/lyric-all"
```

### 播放控制

```bash
curl "$LX_API_URL/play"
curl "$LX_API_URL/pause"
curl "$LX_API_URL/skip-next"
curl "$LX_API_URL/skip-prev"
curl "$LX_API_URL/seek?offset=30"
curl "$LX_API_URL/volume?volume=80"
curl "$LX_API_URL/mute?mute=true"
curl "$LX_API_URL/mute?mute=false"
curl "$LX_API_URL/collect"
curl "$LX_API_URL/uncollect"
```

### 搜索播放（Scheme URL）

运行时应通过内置工具打开完整 Scheme URL，不使用 PowerShell，不打开裸 `lxmusic://`。

示例：`lxmusic://music/searchPlay/晴天-周杰伦`

随后必须轮询状态，确认 `status=playing` 且 `name` / `singer` 匹配。

### 歌单操作（Scheme URL）

示例：
- 播放歌单：`lxmusic://songlist/play/wy/2244784586`
- 播放歌单第 N 首（从 0 开始）：`lxmusic://songlist/play/tx/22928978?index=0`

URL 格式：`lxmusic://songlist/play/{source}/{id}`

source 取值：`kw` | `kg` | `tx` | `wy` | `mg`

## 常见问题

- **API 返回空**：确认 LX Music 已开启「开放 API 服务」（需要 v2.7.0+）
- **Scheme URL 无法打开**：确认 LX Music 已正确安装并注册协议；运行时优先通过可执行文件参数打开完整 Scheme URL
- **端口被占用**：LX Music 设置中可修改 API 端口
- **歌单 ID 查找**：运行 `python scripts/playlist.py` 查看所有歌单
