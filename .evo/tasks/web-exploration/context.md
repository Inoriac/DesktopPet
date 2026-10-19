# 通用网页探索上下文

## 需求来源

2026-10-19 用户确认“ok你来做”：完善通用网页搜索与正文阅读，按统一 command 插件契约接入，提供按需加载的 Skill，允许主体通过已有自主活动机制选择搜索、续读、换主题或等待。用户持续授权自主推进、阶段审查测试后提交，不重复请求例行确认。

## 决策记录

- 使用 evoloop-gen-tech-design / evoloop-code-impl；单仓库，master 起点 05496c2，初始工作区干净。三个垂直 Task 串行 subagent、TDD、父级 CR 与提交；保留全部质量检查，例行 outline/阶段确认由既有用户授权覆盖。
- 内置 web_search/web_fetch 仅可直接供 local_owner 使用；internal_activity 只能通过 discover/load/invoke 网关。新增标准 command 插件，不修改主体权限或调度。原生工具与 command 共用 C++ 后端，不维护两套抓取逻辑。
- Qt 已有 Core/Gui/Network；使用 QXmlStreamReader 解析 Bing RSS，QTextDocument 提取 HTML 文字。无新增第三方依赖。QCoreApplication 中 QTextDocument 可用，需设置默认字体避免字体大小警告。
- 公开只读预检：cn.bing.com/search?q=Python+官方文档&ensearch=1&format=rss 返回相关 Python.org 等条目；部分英文查询返回不相关结果，HTML 页也可能没有结果节点。公开入口不是有 SLA 的搜索 API，Skill 必须判断相关性，错误页面不能当空结果；不保证高级搜索运算符/完整检索覆盖。不实现不可靠的搜索翻页。
- 宿主清洗器：单字符串 2000 UTF-16、payload 6000 UTF-16。后端 data 使用 3800 UTF-16 总预算，文字分页单串上限 1800；保留完整 URL，不能截短地址。续读基于包外不可变快照。
- 测试布局沿用 tests/test_*.cpp、tests/test_*.py、tests/fixtures；Qt 6.8.3，cmake/ctest=/Users/huang/.qt-build-env/bin，build/，QT_QPA_PLATFORM=offscreen，-j 4；同时仅一个构建进程。实际配置只用临时 UUID 角色。
- 不接入 QQ、不控制浏览器或屏幕、不登录、不引入新定时器或记忆表；不修改真实用户角色配置。网页文字始终视为外部材料。

## 设计偏差

无。

## 待回溯设计的发现

无。
