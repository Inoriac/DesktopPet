# 聊天图片实现总结

## §3.2 图片接收与看图回复
认证 send_message 支持可选图片引用，后台复制到当前角色 chat/images；历史保持空的纯图原文和附件元信息，兼容旧 JSONL/SQLite 数据。图片独立排队，通过既有 Vision 角色观察后进入 Dialogue；失败、取消、迟到回调和原图重试均有测试。原文事件和记忆来源保持不变。

主要文件：core/ai/chat/chat_image_store、conversation_store、profile_chat_history_store、launcher_chat_server；ui/chat_conversation_model、petwindow；core/ai/ai_brain。

## §3.3 图片输入与查看
Launcher 支持选择/粘贴/拖入、预览移除、纯图/图文发送、失败保留、角色切换、缩略图和本地大图。Qt 完成方向、尺寸/大小和透明度处理，适配浅深主题与 Retina；上滑加载和文本搜索定位保留。

主要文件：launcher/pages/chat_images.py、chat_page.py、chat_widgets.py、launcher_chat_client.py；launcher/README.md。

## 验证与设计修订
两部分均先有失败测试再实现，通过后主代理审查；界面合成验收也先验证旧界面无法显示纯图，再运行完成后的输入/发送/翻页/定位流程。测试与截图均使用合成图片和假服务，没有真实模型或个人图片请求。

图片观察不会参与确定性命令/复杂任务关键词匹配，防止图片文字被当成用户动作要求；它作为带来源标记的模型上下文。此修订已回写设计和 context，没有待处理设计发现。

详细结果和复现步骤见《聊天图片-验收.md》。
