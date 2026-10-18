# Launcher 聊天重构实现总结

## 交付
- §3.2：完整历史按消息 ID 分页、字面子串搜索及上下文查询；只读当前 profile 的 local_owner，复用 SQLite/JSONL 与认证管道。异步查询排在终态持久化之后，正常快照最近 40 条，诊断单独提供。
- §3.3：QQ 私聊式标题、头像、蓝/白气泡、时间分隔与输入区；上滑每次取 40 条，保持阅读位置，保留草稿、停止回复、忙时发送与任务操作。
- §3.4：可折叠搜索、30 条结果分页、点击上下文定位/高亮、Ctrl+F/Esc、返回最新。宽屏靠右，窄屏放顶部；迟到响应不会污染新查询或角色。

## 主要文件
- core/ai/chat 的 chat_types、conversation_store、profile_chat_history_store、launcher_chat_server。
- ui/chat_conversation_model、ui/petwindow.cpp；launcher/launcher_chat_client.py。
- launcher/pages/chat_page.py、chat_widgets.py；launcher/README.md。
- tests 目录的历史/model/server/launcher/transport 测试，CMake 新增 server 套件。
- 本目录设计、实施计划、审查、搜索验收和 visual-qa 合成演示/截图。

## 设计与 TDD
三个 Task 全部完成。各阶段均先记录行为 RED，再实现 GREEN，并由主代理审查后提交。
实现中补充的只读事务、可见消息筛选、关联查询错误与窄屏布局已同步设计/context，无未处理设计偏差。
最终回归顺带修正一个原有异步测试的等待条件，未扩展产品行为。

## 验证与边界
全构建成功，最终 CTest 63/63；聊天 UI 与管道 45/45。
180 条合成记录的分页、搜索、定位、回到最新及浅/深主题、1100/700/460 宽度验证通过。
macOS 原生完整 launcher 窗口检查通过，截图见 visual-qa/Launcher-Light-1100.png。
本次未新增第三方依赖、数据库或 QQ 接入。新版 Windows UI 需按 launcher/README.md 清单进行本机验收；
先前 Windows 报告中正式配置旧模型名返回 unknown provider 的问题不由本次 UI 改动处理。
