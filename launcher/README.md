# DesktopPet 启动器 (Python)

PySide6 + PySide6-Fluent-Widgets 实现的 Fluent Design 启动器。
负责配置管理 + 指定角色启动，通过命令行参数 `--config` / `--pet` 唤起 C++ 核心。

配置结构以 [默认模板](../config/default_common_config.example.json) 为准；导出时保留未在界面中编辑的字段。

## 运行

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r launcher\requirements.txt
.\.venv\Scripts\python.exe launcher\main.py
```

> 本项目未使用 PySide6-Addons（WebEngine/3D），只装 Essentials 即可，省 ~300MB 下载。

## 前置

- C++ 核心须已构建到 `build/`；支持单配置产物和 `build/Release`、`build/Debug` 等多配置产物。
- 启动器以 `--config <绝对路径> --pet <角色名> --profile-id <uuid>` 启动核心。
- Windows 下配置保存在 `%APPDATA%\Desktop Pet Team\Desktop Pet\launch_config.json`。
  AI 页面可显式保存配置，launcher 下次启动时会自动恢复用户字段。

## 主题共享

启动器启动时 `setOrganizationName/ApplicationName` 逐字对齐 C++ `main.cpp`，
经同名 QSettings（键 `ui/theme`）与 C++ 共享主题，无需配置文件中转。

## 文件

| 文件 | 作用 |
|------|------|
| `main.py` | 主入口：FluentWindow 框架 + 导航 + 启动按钮 + 主题共享 |
| `app_state.py` | 各页配置的运行期状态（dataclass） + 字段映射 settings dict |
| `config_loader.py` | 模板加载 + 用户字段覆盖 + 导出 launch_config.json |
| `pet_registry.py` | 读写 C++ `Pet` 注册表（AppData/pets.json） |
| `pages/` | 宠物/AI/语音/气泡/高级/关于 各设置页 |
| `pages/_ui.py` | Fluent 页面、分区和设置行布局助手 |
| `requirements.txt` | 依赖清单 |

## 聊天与历史记录

聊天页采用私聊布局：双方头像与气泡、时间分隔、独立输入区。Enter 发送，
Shift+Enter 换行；回复过程中仍可继续发消息，也可停止回复。任务操作保留在聊天区上方，
技术诊断在“调试信息”中展开。

- 初次只显示最近 40 条消息；向上滚到顶部才请求更早的 40 条，保留当前阅读位置。
- 阅读旧消息时，新回复不会把页面拉到底部；点击“回到最新”恢复最近消息。
- 点击“聊天记录”或按 Ctrl+F，输入关键词并按 Enter 搜索当前角色已保存的全部对话。
  支持中文、英文大小写不敏感的字面子串；`%` 和 `_` 也按普通字符匹配。
- 搜索结果每页 30 条，点击“加载更多结果”继续查找；点击结果显示附近消息并短暂描边，
  可以继续上滑查看更早上下文。“回到最新”退出历史上下文，Esc 关闭搜索面板。
- 宽页面使用右侧搜索栏，较窄页面将其放在聊天区上方，关闭后恢复完整聊天空间。

这只是本地聊天界面的样式和记录查询，未接入 QQ。查询通过已有认证管道完成，
范围固定为当前角色的本地对话；不会将其他角色或外部会话的消息混入结果。

## Windows 新聊天页验收

`WINDOWS_VALIDATION_2026-10-18.md` 和 `MODEL_VALIDATION_2026-10-18.md` 记录的是本次 UI
改动之前的工具、模型验证，**不代表下面的新聊天界面已经在 Windows 验收过**。
拉取新代码并重新构建 C++ 核心后，可按以下顺序检查：

1. 先运行无需模型、账号或个人聊天数据的测试与合成演示（仓库根目录，已安装 launcher 依赖）：

   ```powershell
   $env:QT_QPA_PLATFORM = "offscreen"
   .\.venv\Scripts\python.exe -m unittest discover -s tests -p "test_launcher_chat*.py"
   .\.venv\Scripts\python.exe .evo\tasks\launcher-qq-chat\visual-qa\render_chat.py
   Remove-Item Env:QT_QPA_PLATFORM
   ```

   演示以 180 条合成记录执行真实分页调用、两页搜索、定位和返回最新，生成浅/深色、
   宽/窄布局截图与 `evidence.json`。它使用内存测试服务，不会连接或更改个人历史。
2. 正常启动 launcher 和桌宠，进入“聊天”，确认头像、左右气泡、输入框和主题可读；
   在 Windows 100%/150% 缩放、约 1100/700 像素宽窗口下检查布局，必要时关闭搜索栏。
3. 使用已有较长会话：打开时只见最近一页，上滑才补旧页，当前消息不跳动；
   连续上滑直到起点，确认不重不漏。窗口缩放本身不应触发连续加载。
4. 搜索首屏之外的一段已知中文，再试英文大小写、字面 `%`/`_`、无匹配词；
   命中超过 30 条时继续翻页。点击结果应看到原消息及邻居，命中短暂描边，继续上滑可读更早记录。
5. 阅读历史时接收一条新回复，确认仍停留原处；点击“回到最新”能看到新消息。
   搜索栏中连续换关键词、连续点不同结果、切换角色后，不能出现旧查询结果或上一角色消息。
6. 检查 Enter/Shift+Enter、忙时发送、停止回复、失败保留草稿及任务继续/停止按钮；
   搜索打开时这些功能仍应可用。断开桌宠后应保留已显示的内容，禁用在线操作，重连后可恢复。
7. 搜索框或聊天输入中按 Ctrl+F/Esc；切到其他设置页后快捷键不应打开隐藏的聊天搜索。

macOS 合成截图与执行记录位于
[`visual-qa`](../.evo/tasks/launcher-qq-chat/visual-qa/)，阶段验收说明见
[`聊天记录搜索-验收.md`](../.evo/tasks/launcher-qq-chat/聊天记录搜索-验收.md)。
