# Windows 验证报告后续修复（2026-10-05）

基线为 `abc6e6c` 中的 `windows_launcher_acceptance_2026-10-05.md`。本轮先处理 Launcher，再处理主程序自动聊天与错误通知的连接。按用户要求，不处理模型转换引起的动画姿态问题。

## 修复结果

| 原问题 | 本轮处理 | 验证边界 |
| --- | --- | --- |
| 连通测试期间其他点击无响应、Launcher 容易退出 | 找到界面线程同步等待聊天 IPC 的确定阻塞路径，改为异步连接、读取和发送；每个完整响应有总超时，轮询合并、命令串行、旧连接回调失效。API 连通测试另补总超时、取消、页面销毁保护、无效 URL/header 反馈及传输中断判断 | 真实 Qt 事件循环和回环服务验证慢响应时界面定时器、页面切换仍工作；原 Windows 闪退未在本机复现，不能宣称所有崩溃原因均已消除 |
| 聊天历史占满页面、恢复后新回复不可见 | QQ 式左右气泡；默认仅显示最近 40 条，按需加载当前同步范围内的更早记录；按消息 ID 更新；修复布局完成时机与换行高度导致的底部空白 | 当前核心仍同步最近 120 条历史；没有删除更早的持久化记录，也未新增无限历史查询接口 |
| 滚动体验 | 常规右侧滚动条；上翻时保留阅读位置；新增消息不移除仍在阅读的起始消息；离开底部出现“返回底部”；显式返回底部覆盖旧的延迟滚动恢复 | Qt 实际几何断言验证最新消息在视口中，另查看离屏渲染截图 |
| 忙碌时回车变成停止，草稿未发送 | 发送和停止独立；有草稿时回车先请求停止旧回复，观察到空闲后自动发送；收到发送确认前保留草稿，失败可重试 | 覆盖停止与提交顺序、只发送一次、发送失败保留草稿、Shift+Enter 换行 |
| 聊天中混入系统信息 | 默认隐藏调用统计，放入可展开的调试区；系统消息和错误细节不进入对话气泡；新失败用可关闭、非模态通知显示原因并提供重试，旧历史失败不反复弹出 | 上游请求拒绝也改成 Failed + errorMessage，不再伪装为正常 assistant 正文。独立桌宠使用非模态通知；Launcher 有认证连接时由 Launcher 提示 |
| quiet/focus 仍出现 idle_action 文字 | 统一约束 idle_action、proactive_chat、emotion 自动回合；准备、流式、完成、工具执行和确认回调再次检查模式，用户输入可以抢占自动回合 | 手动输入、触摸、手动屏幕互动及明确设置的提醒仍可用；没有修改模型动画 |
| 默认启动旧 Release | 在兼容的候选产物中选择修改时间最新者；显式 DESKTOP_PET_EXECUTABLE 优先；启动按钮提示可查看实际核心路径 | 时间选择不能代替构建版本校验，Windows 复验前仍应构建最新源码 |
| macOS 主程序连接名称过长 | Launcher 使用带用途前缀的短随机 socket 名，C++ bootstrap 校验兼容新旧格式，保留 profileId 和认证 token 验证 | 已验证 macOS 实际本地 socket listen 与 OwnerDiary bootstrap |

## 验证

- 主程序 `Desktop_Pet` 与受影响 C++ 测试目标构建成功；`DESKTOP_PET_ENABLE_ONNX_RUNTIME=OFF`，主程序动态依赖不含 ONNX。
- 14 组回归最终通过：LauncherConfig、PetProfileId、LauncherProcessTracker、ModelRoleConfig、ApiConnectionTester、LauncherChat、LauncherChatTransport、OwnerDiaryServer、ChatUiModel、StreamingDialogue、ModuleConnectivity、ScheduleConnectivity、AgentScheduler、ScheduleTools。
- 初次统一回归发现聊天气泡换行高度造成底部空白；修复并增加“消息实际可见”断言后，单独复跑 LauncherChat 全部通过。其他 13 组已通过且对应实现未再修改。
- 真实 Qt 定向测试包括 API 连通测试 17 项、聊天页面 10 项、本地传输 10 项；慢速、分片、超时、断线、销毁、切页等使用本地可控服务，不使用真实外部模型。
- 桌面交互工具启动 Python 时被 macOS 对 Desktop 目录的访问权限阻止，因此没有宣称完成本轮桌面点击验收。整窗离屏启动又遇到第三方 macOS 无边框库调用原生窗口接口的崩溃，已定位为不支持 offscreen 的测试限制，没有据此修改 Windows 窗口实现。聊天面板本身的真实 Qt 离屏渲染和事件测试已完成。
- 配置与聊天测试数据隔离在 `build/launcher-qa`，没有覆盖用户角色注册或聊天数据。构建、回归日志为 `build/windows-followup-build.log`、`build/windows-followup-tests.log`；截图为 `build/launcher-qa/chat-latest.png`、`chat-scrolled.png`、`chat-error.png`。

## Windows 复验建议

1. 重建核心后，从 Launcher 启动，查看启动按钮提示确认实际 EXE。进行连通测试时切换 AI/聊天/其他页面，随后关闭窗口，确认没有卡死或退出异常。
2. 恢复长历史，确认初始显示最新消息；上翻后等待新消息，阅读位置应保持；点击返回底部，最新消息应在视野内。
3. 回复过程中输入新草稿并按回车，应中断旧回复并自动提交草稿；单独停止不能发送草稿。
4. 使用无效测试连接制造失败：错误应出现在独立提示，聊天内容不含网络堆栈；技术细节只在调试区，重试入口可用。
5. 设置临时 quiet/focus，确认空闲动作不产生文字，同时明确设置的到期提醒仍可送达。

报告中的远端 `Connection closed` 无法仅凭日志判断客户端或服务端根因。已核查主动取消与网络错误的分类，未盲目添加可能重复执行工具的重试。真实模型稳定性、独立长期召回质量及报告列出的未覆盖实机场景，仍不能用本轮替身服务测试代替结论。
