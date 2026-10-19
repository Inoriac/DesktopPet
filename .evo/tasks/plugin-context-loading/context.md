# 插件按需上下文上下文

## 需求来源
2026-10-19 用户确认上一轮五点方案，并明确“可以，你来做吧”。方案范围：摘要发现、公共说明与操作分离、每请求按需组装、说明不进入生活记忆、保留完整执行日志。沿用此前逐阶段 commit、自主推进的授权；无需重复阶段确认。单仓库 /Users/huang/Desktop/DesktopPet，master，起点 48c3e80，开始时干净。

## 决策记录
- [2026-10-19] 使用 evoloop-gen-tech-design / evoloop-code-impl。用户已确认方案、实施顺序及自主推进；顺序实现并 review、TDD、Wave 验证，确认不重复询问。
- [2026-10-19] 只修改宿主上下文与发现机制，Plugin/Skill/Tool 格式 1、command/MCP 执行、权限和回执契约保持兼容。无用户配置修改，无真实模型付费调用。
- [2026-10-19] 环境：cmake=/Users/huang/.qt-build-env/bin/cmake，ctest 同目录；Qt 6.8.3，build/；Python launcher/.venv/bin/python；-j 4；QT_QPA_PLATFORM=offscreen。禁止 agent 间并发构建。

## 设计偏差
无。

## 待回溯设计的发现
无。
