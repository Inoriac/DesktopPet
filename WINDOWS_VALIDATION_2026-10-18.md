# Windows 验证记录（2026-10-18）

## 范围与环境

基线 `d4ca21d`，检查最近约 20 个提交，重点为 `375cc51` 至 `d4ca21d`：人物目录/记忆归属、多会话、插件运行时、自主活动/预算/选择、外部观察与自主聊天、LX Music 插件，以及语音、模型回调和原生音乐修复。

本机 Windows 11 家庭中文版 10.0.26100，Qt 6.5.3、MinGW 11.2、Release、Python 3.12.4。复用 `cmake-build-release-mingw_qt`，重新配置并编译当前源码；开启本机 ONNX Runtime。libsodium/QtKeychain 未安装，私密反思生产能力仍关闭。契约验证使用现有 Anaconda Python 与 jsonschema 4.19.2，没有安装依赖。

## 最终结果

- 完整 Windows 构建、修复后增量重建成功。
- 最终完整 CTest **62/62 组通过，70.36 秒**，JUnit：`build/windows-validation-ctest.xml`。
- 通用契约 **96/96 通过**，日志：`build/windows-validation-contract.log`。
- 真实播放器 C++ 网关 opt-in 测试单独通过（普通 CTest 跳过此项，避免默认操作真实软件）。
- 音乐 Python 套件 24 项中有 6 项 POSIX 假可执行文件用例按原设计在 Windows 跳过；Windows 参数构造用例与真实 EXE/Scheme 实测另外通过，不把跳过项计为已验。
- `git diff --check` 通过；未提交或推送此次修改。

## 验证中发现并修复

1. 受控聊天示例跨 Windows 盘符检查抛出 `ValueError`。跨盘数据库属于包外，应允许；仍拒绝包内日志。
2. 受控聊天 source 用 `select` 等待 stdin 管道，Windows 不支持。改用有界队列和独立读线程，保留实时监听、ack、stop、EOF 与游标语义。
3. 受控聊天继承 GBK 标准输入输出，UTF-8 中文/emoji 会解码失败。适配器显式使用 UTF-8，配置生成器同样输出 UTF-8；强制 GBK 父环境的中文/emoji 输入输出实测通过。
4. 契约和音乐 Python 测试把 SQLite 事务上下文误当连接关闭，Windows 删除临时文件报 WinError 32。显式关闭连接，保留事务提交。
5. 真实 LX 2.12.2 的歌曲表是 `(id,listId)` 唯一；原查询仅按 id JOIN，跨歌单重复匹配。新插件和旧歌单脚本均按 listId 关联；兼容不带 listId 的旧表，新增当前表结构回归。
6. PluginRuntime 初始化读取 `plugins.json` 后，未关闭读句柄就原子重写同一文件。Windows 返回“Plugin settings could not be saved”，阻止实际插件加载。解析后先关闭文件，真实网关和受控集成复验通过。
7. 插件、自主聊天、外部观察与模块连通测试硬编码 `/usr/bin/python3`。改由 CTest 传入 CMake 找到的 Python，直接运行时使用平台对应的 PATH 查找。
8. 日记管道测试的客户端和服务端在同一线程，阻塞等写完成会卡住 Windows 服务端事件处理。测试改为泵事件并等待实际字节排空；不修改生产认证与 ACL 逻辑。

首次离屏 UI 测试没有部署 qoffscreen.dll，因此停在 Qt 平台插件加载阶段。指定本机 Qt 的 platforms 目录后运行正常。首次并行 Python 回归中的 API 测试等待超时，单独及后续完整套件复验通过，未据此修改业务代码。

## 真实 LX Music

LX Music 2.12.2.0，程序 `E:/lx-music-desktop/lx-music-desktop.exe`，数据库 `%APPDATA%/lx-music-desktop/LxDatas/lx.data.db`，API `http://127.0.0.1:23330`。

- 诊断为 ready；真实 status、lyric、list_playlists、playlist_songs 成功，数据库只读访问；返回两个用户歌单。修复前发现重复曲目，修复后与 UI 列表顺序核对。
- play/pause 实际改变播放状态，进度前进；skip_next/skip_prev 各发一次并观察曲名变化和返回。
- search_play 通过 EXE/Scheme 搜索并播放目标歌曲；play_playlist 使用数据库返回的来源 ID，开始播放所选歌单首曲。
- 关闭此次启动的播放器后，status 失败且不自动启动；显式 launch 成功启动 EXE，并确认 API 就绪。
- set_volume 将设置从 10% 改为 30%，真实播放器保存配置确认 0.3；随后恢复 10%。该版本 `/status` 没有 volume 字段，因此适配器在约 8 秒后保守返回 unknown，未伪报 completed，也没有自动重复控制。此项属于已验证的 API 兼容限制。

原始调用记录：`build/windows-real-music.jsonl`（包含本机歌单，不提交）。用户角色配置未改动；用于网关验收的配置单独放在 `build/windows-live/profiles/12345678-1234-4234-8234-123456789abc`。

测试结束将播放器恢复到原曲名、暂停和 10% 音量，并关闭此次启动的播放器。测试期间播放器自行提示并下载新版；没有安装更新。

真实 C++ PluginRuntime 的 discover/load/invoke 验收通过：找到 Skill 和全部 12 个操作；经网关实际读取 status、lyric、两个歌单和第一个歌单的 320 首歌曲。日志 `build/windows-live-gateway.log`。新增 opt-in `real_player_read_only_gateway`，普通回归默认跳过，设置 `DESKTOP_PET_LX_ACCEPTANCE_HOST` 为隔离配置文件后单独执行；此测试只读取播放器，不播放。

## 真实语音

使用本机已有 GENIE 模型和 feibi 角色、禁止自动下载。真实 worker 依次产生 ready、configured、speech_started、speech_finished、shutdown；生成 197120 字节音频，WAV 为 32000 Hz、98560 帧、3.08 秒，播放调用正常结束。未用人工听辨评价音质。

日志：`build/windows-real-voice.jsonl`、`build/windows-real-voice-stderr.log`；音频：`build/windows-voice-audio/desktop_pet_voice_windows-smoke.wav`。受控进程中的配置更新、异常退出、重启和队列清理另由 VoiceSynthesisTests 验证。

## 桌面启动

使用新构建的 Desktop_Pet.exe、单独关闭 AI/自主活动/屏幕聊天/语音的测试配置启动已注册 Milltina；实际窗口成功加载并渲染 3D 角色，随后通过 Alt+F4 正常退出。没有进行真实模型请求。日志 `build/windows-gui-stdout.log`、`build/windows-gui-stderr.log`。

## 复现命令

```powershell
$env:PATH = 'E:\Qt\Tools\mingw1120_64\bin;E:\Qt\6.5.3\mingw_64\bin;' + $env:PATH
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = 'E:/Qt/6.5.3/mingw_64/plugins/platforms'
cmake -S . -B cmake-build-release-mingw_qt
cmake --build cmake-build-release-mingw_qt -j 8
ctest --test-dir cmake-build-release-mingw_qt --output-on-failure -j 2 --timeout 90 --output-junit E:/Funny-Projects/Desktop-Pet/build/windows-validation-ctest.xml
$env:PYTHONUTF8 = '1'
python contracts/tools/verify_contract.py
```

编译日志 `build/windows-validation-build.log`、`build/windows-validation-rebuild.log`；完整测试日志 `build/windows-validation-ctest.log`。全量编译有两处已有的 Qt 矩阵运算弃用警告，未作为本次验证问题修改。

## 边界

后续已补充用户指定 `gemini-3.8-flash-high` 的真实模型行为验收，并修复响应 schema 未发送给模型的问题，详见 [模型验收记录](MODEL_VALIDATION_2026-10-18.md)。其中音乐模型验收使用隔离后端，真实播放器验证仍以本报告上述记录为准。

不把受控 provider / 本地聊天模拟器通过等同于真实 QQ 登录或真实模型行为质量。没有向外部账号发送消息。真实桌宠长时间交互、帧时间与人工听感未据此宣称通过。
