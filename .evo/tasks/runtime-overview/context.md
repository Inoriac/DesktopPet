# 运行概览上下文

## 需求来源

2026-10-20 用户希望检查并改造已有统计、日志系统，在 launcher 有独立界面查看 token 消耗、心情等基本运行属性。用户此前授权自主推进、按阶段 commit，继续有效；不逐阶段等待确认。使用 evoloop-gen-tech-design 和 evoloop-code-impl，保留串行子代理、TDD、审查与阶段验证。

## 决策记录

- 工作区 `/Users/huang/Desktop/DesktopPet`，单仓库，开始时干净，基线 `84e6b34`。无 AGENTS.md，evo CLI 不可用，使用本目录记录设计。
- 旧 StatisticManager 可复用，但按名称写全局 log/statistics.json；多角色覆盖和改名归属有问题。旧 AI 详细日志也共用 log/ai_calls.jsonl，缺轮转。角色已有不可变 profileId。
- launcher 使用 PySide6 + Fluent Widgets；复用布局和本地认证管道，新增只读运行查询，不复用整页聊天快照做高频统计。
- 真实模型和私人 QQ 不参与验证。使用临时目录、假模型、本地 socket、合成 UI 数据；Windows 实机步骤交接。已知 WebCommandTests 在 macOS 被 AMFI/XProtect 终止，不在最终本地全套通过声明中包含该项。
- 测试工具：`/Users/huang/.qt-build-env/bin/{cmake,ctest}`；Qt `/Users/huang/Qt/6.8.3/macos`；launcher Python 为 `launcher/.venv/bin/python`。新增测试遵循根目录 tests/test_*.cpp 和 tests/test_*.py。

## 设计偏差

无。

## 待回溯设计的发现

无。
