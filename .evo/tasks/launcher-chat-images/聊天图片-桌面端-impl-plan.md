# 实施计划

本文件为进度唯一来源。设计：聊天图片-桌面端-系分.md。用户授权自主推进与分阶段提交。

## Task 1: 图片消息接收、保存与看图回复
- 关联设计：§3.2；状态：[x]；Wave：1；依赖：无。
- 文件：core/ai/chat/chat_image_store.{h,cpp}、chat_types/conversation_types/conversation_store/profile_chat_history_store/launcher_chat_server；ui/chat_conversation_model、ui/petwindow；core/ai/ai_brain相关文件；CMakeLists；tests/test_chat_history.cpp、test_chat_conversation_model.cpp、test_launcher_chat_server.cpp、test_streaming_dialogue.cpp或仓库现有AIBrain测试、新增test_chat_images.cpp。
- RED/GREEN：副本持久化与来源删除、纯图/图文历史分页、元信息幂等与scope、JSONL兼容/存储错误清理；认证异步send；Vision图像块→主体上下文、文字不误变原话；失败/停止/迟到/排队/重试与旧文字回归。
- sanity check、主代理审查、相关及完整构建/测试后提交。

## Task 2: Launcher 图片编辑与历史显示
- 关联设计：§3.3；状态：[ ]；Wave：2；依赖：Task 1。
- 文件：launcher/launcher_chat_client.py、pages/chat_page.py/chat_widgets.py/chat_images.py；tests/test_launcher_chat.py、test_launcher_chat_transport.py、可选test_launcher_chat_images.py 与其 CMakeLists.txt 注册；launcher/README.md；本任务验收/QA产物。
- RED/GREEN：选图/粘贴/拖入、预览移除、纯图与图文发送、原图规范化/格式数量边界、成功清理失败保留/在途修改/角色切换；历史缩略图/大图/丢失文件、滚动与搜索回归；浅深/窄屏合成视觉与完整launcher验收。
- 主代理最终审查、全量CTest、总结后提交。
