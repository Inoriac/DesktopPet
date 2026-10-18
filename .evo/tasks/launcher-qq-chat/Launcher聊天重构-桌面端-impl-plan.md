# 实施计划

用户授权自主推进、每部分提交；本文件为进度唯一来源。设计：Launcher聊天重构-桌面端-系分.md。

## Task 1: 历史查询与管道协议
- 关联设计：§3.2；状态：[x]；Wave：1；依赖：无。
- 文件：conversation_store、profile_chat_history_store、chat_types、chat_conversation_model、launcher_chat_server 的 h/cpp；ui/petwindow.cpp；launcher/launcher_chat_client.py；tests/test_chat_history.cpp、test_chat_conversation_model.cpp、test_launcher_chat_transport.py、必要的 test_launcher_chat_server.cpp 与 CMakeLists.txt。
- 测试：超过120条多页不重不漏/同时间戳/追加后游标稳定；全文搜索中文/%/_及页尾；around邻居；跨profile/外部会话拒绝；JSONL兼容；异步非阻塞及销毁；snapshot40和流式；认证查询协议及请求关联。
- 必须先 RED 后实现；主代理审查后提交。

## Task 2: QQ 会话页与上滑加载
- 关联设计：§3.3；状态：[x]；Wave：2；依赖：Task 1。
- 文件：launcher/pages/chat_page.py、可选 chat_widgets.py；tests/test_launcher_chat.py。
- 测试：首屏40/初始化不加载旧页；用户上滑单次分页；视口锚点；新消息不抢历史；合并流式不丢旧页；角色切换丢旧请求；保留发送/停止/任务/草稿；浅深主题与700/1100宽布局。
- 必须先行为 RED，再实现；视觉仅使用合成内容。

## Task 3: 聊天记录搜索与定位
- 关联设计：§3.4；状态：[x]；Wave：3；依赖：Tasks 1–2。
- 文件：launcher/pages/chat_page.py、chat_widgets.py；tests/test_launcher_chat.py；launcher/README.md；本目录验收记录与合成截图。
- 测试：首屏之外全文搜索/分页；命中上下文定位；迟到结果/换角色；空结果/失败重试；Ctrl+F/Esc；定位模式返回最新和继续上滑；完整回归、浅深主题及窄屏视觉验收。
- 主代理整体核对覆盖与文档一致性，提交并完成总结。
