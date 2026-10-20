# QQ 插件上下文

## 需求来源
用户要求借鉴 https://github.com/Mai-with-u/MaiBot ，改进为适合 DesktopPet 的插件。延续用户对后台 QQ、通用工具契约、自主选择、人物关系/范围隔离与分阶段 commit 的要求。实现不测试真实私人账号或向外发消息。

## 决策记录
- 采用 evoloop-gen-tech-design 与 evoloop-code-impl 工作流。用户先前“你看着往下做吧，每做一个阶段就 commit 一下”及离线自主推进授权持续有效，不再逐阶段询问批准。保持串行 subagent、TDD、review 与验证。
- 当前宿主只支持 text/reply 外部观察。首版用诚实媒体占位，QQ 图像理解/上传不在本次文本接入中宣称完成。复用既有自主机会、冷却及身份/记忆，插件不另建主动回复调度器。
- 研究仓库只读放在 `/tmp/desktop-pet-maibot-review.pEfjaV/`。仅借鉴协议与架构，原创实现，不复制 GPL 代码到本项目。
- Python 3.9.6 可用；测试依赖使用包外 `/tmp` 独立环境，不修改角色账号配置。Qt 测试默认环境没有 websockets 时 QQ 集成需显式传 QQ_TEST_PYTHON；其他测试沿用原 Python。
- 测试布局参照 plugins/bilibili 与 tests/test_bilibili_{adapter,configure}.py / test_bilibili_plugin.cpp。没有 AGENTS.md。

## 设计偏差
无。

## 待回溯设计的发现
无。
