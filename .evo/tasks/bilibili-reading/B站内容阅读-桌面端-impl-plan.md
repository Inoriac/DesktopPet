# B 站内容阅读实现计划

关联设计：[B站内容阅读-桌面端-系分.md](./B站内容阅读-桌面端-系分.md)。三个 Task 串行、每 Task 父级 CR 后提交，统一 Wave 收尾；此文件为进度唯一来源。沿用用户自主推进与分阶段提交授权，不重复询问例行确认。

## Task 1：B 站查询与字幕阅读
- 关联设计：§3.2
- 状态：[x]
- Wave：1
- 依赖：无
- 文件：plugins/bilibili 的 plugin/tool/skill 清单、USAGE.md、SKILL.md、adapter.py、backend.py、requirements.txt、THIRD_PARTY_NOTICES.md、licenses/*；tests/test_bilibili_adapter.py；本 Task 验证记录。
- 测试：status 缺依赖；搜索/信息/评论正常及异常；正确选择第二分 P；字幕长段/emoji 分页全量还原、来源时间不丢；无字幕与失败区分；坏 cursor/账号变化拒绝；UTF-8 CLI；返回体预算；不会执行平台写操作。
- 验证：Python 单测、真实依赖方法核对、父级 CR；先五个读取操作，ASR 操作由 Task 2 补入。

## Task 2：可选本地语音转写
- 关联设计：§3.3
- 状态：[ ]
- Wave：1（实际严格依赖 Task 1 串行）
- 依赖：Task 1
- 文件：plugins/bilibili/transcription.py、asr_worker.py、requirements-asr.txt、adapter.py、tool.json、skill.json、USAGE.md、SKILL.md；tests/test_bilibili_transcription.py、tests/fixtures/bilibili_asr_engine.py（必要）；本 Task 验证记录。
- 测试：缺配置/模型/依赖不启动；超长拒绝；受理/回执/去重；受控真实后台工作生成 timed segments，查询并分页；无语音；下载/引擎错误、超时、未知状态；临时音频清理与路径隔离。
- 验证：相关 Python 测试，父级 CR。

## Task 3：安装、角色配置与自主接入
- 关联设计：§3.4
- 状态：[ ]
- Wave：1（实际严格依赖前两 Task 串行）
- 依赖：Task 1、Task 2
- 文件：plugins/bilibili/setup.py、login.py、configure.py、README.md；必要的 plugin/tool/skill/USAGE/SKILL/requirements 微调；tests/test_bilibili_setup.py、test_bilibili_configure.py、test_bilibili_plugin.cpp、tests/fixtures/bilibili_backend.py；tests/test_autonomous_chat.cpp、CMakeLists.txt、contracts/tools/README.md、现有工具契约适配验证.md；本 Task 验证记录。
- 测试：准备默认不下模型/显式 ASR；受控登录成功和超时不泄露凭证；配置预览、合并、幂等、备份失败回滚、autonomous on/off；真实网关全操作清单、字幕实际 payload、ASR 回执；禁用/外部上下文拒绝；真实 AIBrain 候选与自主阅读、wait 不强制执行。
- 验证：Python、Qt 相关测试、父级 CR。

## Wave 验证
- [ ] 串行文件冲突核查
- [ ] 全项目构建与全量 CTest
- [ ] 独立契约检查及包结构
- [ ] 设计行为与测试覆盖映射
- [ ] 测试位置/CMake、实现锚点与 context 留痕
- [ ] 真实只读/转写可用条件验证及限制记录
- [ ] 实现总结与最终提交
