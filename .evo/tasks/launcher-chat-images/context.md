# Launcher 聊天图片

## 需求来源
用户在 QQ 风格聊天重构完成后要求支持发送图片。沿用此前自主推进、按阶段 commit 的授权；不等待常规方案确认。
基线 b33e079，工作区 /Users/huang/Desktop/DesktopPet。旧任务 launcher-qq-chat 已完成，本任务单独记进度。

## 工作约定
- 使用 evoloop-gen-tech-design / evoloop-code-impl；两个 Task 串行代理执行，先 RED 后 GREEN，主代理审查与提交。
- 无新增第三方依赖、无真实模型付费请求、无个人图片作演示；合成图片/假模型做链路验证。
- Qt/CMake：/Users/huang/.qt-build-env/bin/{cmake,ctest}，build/；Python launcher/.venv/bin/python。
- 普通控件测试 QT_QPA_PLATFORM=offscreen；完整 launcher 在 macOS 用 cocoa，无边框主窗口不支持 offscreen。

## 决策记录
- 已有 ChatMessage.contentBlocks 支持 text/image，OpenAI-compatible 与 Anthropic 客户端均有图片编码；已有 ModelRole::Vision，Dialogue 默认不支持视觉。因此先由 Vision 解释当前图片，再交原聊天/任务流程，不改变角色模型配置。
- 图片是当前 local_owner 消息的附件，不是 QQ 插件，不开放外部会话读取本地附件。
- 界面提供文件选择、粘贴、拖入；一次最多4张。发送版缩小至最长边2048并编码JPEG/PNG，每张最多2MiB；原输入文件最多20MiB/4000万像素，超限和无效图片给明确提示。历史保存实际发送的版本。
- 通过已认证本机管道传临时文件引用，主体异步复制到当前 profile 的附件目录。快照/历史只传元信息，不在轮询中重复传base64，不放宽1MiB协议帧限制。
- 视觉描述作为可能不准确的图片观察，不能冒充用户原话写入记忆；失败/取消沿用正常回复状态和重试。

## 设计偏差
无。

## 待回溯设计的发现
无。
