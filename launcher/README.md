# DesktopPet 启动器 (Python)

PySide6 + PySide6-Fluent-Widgets 实现的 Fluent Design 启动器。
负责配置管理 + 指定角色启动，通过命令行参数 `--config` / `--pet` 唤起 C++ 核心。

配置结构以 [默认模板](../config/default_common_config.example.json) 为准；导出时保留未在界面中编辑的字段。

## 运行

```powershell
$basePython = (Get-Command python.exe -ErrorAction Stop).Source
& $basePython --version
& $basePython -m venv .venv
& .\.venv\Scripts\python.exe -m pip install -r launcher\requirements.txt
& .\.venv\Scripts\python.exe launcher\main.py
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
| `statistics_data.py` | 只读角色统计文件、离线日期计算和显示格式 |
| `pages/` | 配置、聊天、运行概览、私人日记和关于等页面 |
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

### 发送图片

输入区点击“图片”选择文件，也可把本地图片拖入输入框，或直接粘贴剪贴板中的截图。
图片先显示为草稿缩略图，点击 × 可移除；按 Enter 或“发送”提交，支持纯图片和文字配图，
每条最多 4 张。发送中仍可编写下一条文字；发送失败、断线时保留当前草稿，切换角色会清除旧附件。

- 支持 Qt 能读取的 PNG、JPEG、WebP、BMP、GIF、TIFF 等格式；动画图片只发送首帧。
- 原文件最多 20 MiB、4000 万像素。自动按拍摄方向旋转、最长边缩至 2048 像素，
  转为每张不超过 2 MiB 的 JPEG/PNG；透明图片保留透明度，必要时进一步缩小。
- 聊天历史保存的是实际发送的图片版本。原文件删除后仍可查看，点击历史缩略图可打开大图，
  上滑加载的旧图片和搜索定位后的图片同样可查看。记录搜索只查文字，不识别图片内的文字。
- 看图使用 AI 设置中的 **Vision（视觉）模型角色**，需要该角色配置支持图片的模型。
  视觉识别完成后由当前聊天主体回答；识别失败时保留图片和重试入口。普通文字聊天保持原流程。

聊天、图片发送和历史查询通过已有认证管道完成，范围固定为当前角色的本地对话；
不会将其他角色或外部会话的消息混入结果，也未接入 QQ。

## 运行概览与日志

在左侧导航选择“运行概览”，查看“宠物”页当前所选角色的统计。所选角色通过本启动器
连接到核心时，页面显示期间每 2 秒刷新；切换到其他页面后停止查询。读取状态不调用模型，
也不推进情绪或触发自主活动。聊天发送、停止和历史查询优先于后台刷新。

- 首屏显示心情、心境、活跃度、情绪强度，以及 AI 开关、忙闲/排队、自主活动、任务、
  运行时长和触摸次数。自主活动可区分进行中、选择活动、冷却、预算等待、暂停和异常。
- 用量可切换“今天 / 本次运行 / 累计”，包括输入、输出、总 token、成功、失败、取消、
  未报告用量的调用，以及缓存和推理子项。一次真实模型请求记一次，重试和备用模型分别计数；
  输入已包含缓存，子项不会重复加入总量。服务没有报告用量时，不把它当作确定的零消耗。
- 下方展示最近 7 日趋势、累计用途/模型明细，和最近 200 条调用元数据。可按模型用途、
  结果、模型/提供方筛选，每页 25 条；自动刷新保留当前范围、筛选和页码。
- 未连接时可以读取已保存统计；“今天”按当前本地日期计算，“本次运行”和心情/活动不可用。
  旧核心不支持运行概览时显示升级提示，并停止重复查询；聊天仍可使用。

统计文件位于 AppData 的 `profiles/<profileId>/statistics.json`，按不可变角色 UUID 隔离，
改名不会丢失累计。核心每 10 秒保存一次并在退出时保存；日汇总保留 90 天。
服务未报告完整用量或异常退出时可能有缺失，因此这些数字用于观察消耗，不替代供应商账单。
只有角色清单中唯一角色且名称精确匹配时才导入旧全局累计，不补造日期或模型明细。
文件损坏、保存失败或同角色重复进程无法取得写锁时，页面会显示告警。

“诊断日志”可打开当前角色的 `profiles/<profileId>/logs/`，以及本启动器保留的核心运行日志。
详细 AI 日志为 `ai_calls.jsonl`，默认 8 MiB 轮转、保留 3 个备份；旧全局日志保留原位置。
页面不自动读取详细日志正文，近期调用表也不保存 prompt 或回复。

无需模型或真实角色数据的验证：

```sh
python tests/test_launcher_statistics.py
python tests/test_launcher_chat_transport.py
python .evo/tasks/runtime-overview/visual-qa/render_overview.py
```

合成脚本默认生成独立页面的浅/深/窄屏、空态、离线和调用记录截图；加 `--native` 可检查真实
launcher 外壳和导航，仍只使用临时数据且不启动核心。Windows 实机与真实模型用量的对照步骤见
[运行概览验收](../.evo/tasks/runtime-overview/运行概览-验收.md)。

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
8. 验证图片：分别用选择文件、粘贴截图、拖入输入框添加图片，检查预览/移除、图文和纯图发送；
   试 4 张/超过 4 张、无效文件，错误应清晰且不丢已选图片。发送中修改文字，成功后新文字应保留。
9. 测试断线后重连同一角色，图片草稿应保留；切换其他角色后不应带上旧角色附件。
   点击历史图查看大图并缩放窗口，重启核心后查看旧图；删除最初选择的原文件不应破坏历史。
   在已配置视觉模型的环境可发送一张非敏感测试图，确认能识别，失败后重试/停止可用。

图片的无模型合成验收脚本是
[render_images.py](../.evo/tasks/launcher-chat-images/visual-qa/render_images.py)，
它生成图片输入、历史与搜索定位的浅/深/窄屏示例；以上 Windows 步骤仍需在目标机器执行。

macOS 合成截图与执行记录位于
[`visual-qa`](../.evo/tasks/launcher-qq-chat/visual-qa/)，阶段验收说明见
[`聊天记录搜索-验收.md`](../.evo/tasks/launcher-qq-chat/聊天记录搜索-验收.md)。
