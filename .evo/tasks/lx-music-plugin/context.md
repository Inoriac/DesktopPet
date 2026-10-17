# LX Music 通用能力接入上下文

## 需求来源

用户：本地没有音乐工具，需要在 Windows 测试；去 Windows 之前完成所有能在 macOS 完成的工作。承接现有工具＋技能契约适配讨论，先完成音乐样板。QQ 明确另立事项。既有授权：自主推进，每阶段 commit，不要求逐项重复确认。

## 决策记录

- 使用自包含标准库 command 插件，保留原生 C++ 音乐功能。工具/技能提供真实操作与方法，不新增核心软件名称分支。
- 拆为两个垂直切片；实现按 evoloop-code-impl 每 Task 一位子代理串行，父级负责设计、审查、完整验证、提交。不得并行编辑或构建。
- 使用 evoloop-gen-tech-design 完成适配设计；父级自行承担没有可用专属 skill 的代码审查角色。
- macOS/Qt 环境：/Users/huang/.qt-build-env/bin/{cmake,ctest}，Qt /Users/huang/Qt/6.8.3/macos，build/；QT_QPA_PLATFORM=offscreen。工作区初始干净，基线 cc3e16d。
- 现有测试布局为仓库 tests/，Python test_*.py 与 C++ test_*.cpp；测试服务在 tests/fixtures/。不操作用户真实角色数据、音乐数据库或播放服务。

## 设计偏差

无。

## 待回溯设计的发现

无。
