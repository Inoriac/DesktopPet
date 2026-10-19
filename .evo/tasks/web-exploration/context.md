# 通用网页探索上下文

## 需求来源

2026-10-19 用户确认“ok你来做”：完善通用网页搜索与正文阅读，按统一 command 插件契约接入，提供按需加载的 Skill，允许主体通过已有自主活动机制选择搜索、续读、换主题或等待。用户持续授权自主推进、阶段审查测试后提交，不重复请求例行确认。

2026-10-19 用户追加：“测试不急，我到时候再做，你留一份需要测试的备注就行。”后续先完成实现、代码审查与必要编译；不继续扩大或重复运行测试。保留已完成结果，新增验收步骤集中到 plugins/web/验证备注.md，由用户之后验证。此明确指示覆盖 skill 的后续 TDD/全量 Wave 测试要求。

## 决策记录

- 使用 evoloop-gen-tech-design / evoloop-code-impl；单仓库，master 起点 05496c2，初始工作区干净。三个垂直 Task 串行 subagent、TDD、父级 CR 与提交；保留全部质量检查，例行 outline/阶段确认由既有用户授权覆盖。
- 内置 web_search/web_fetch 仅可直接供 local_owner 使用；internal_activity 只能通过 discover/load/invoke 网关。新增标准 command 插件，不修改主体权限或调度。原生工具与 command 共用 C++ 后端，不维护两套抓取逻辑。
- Qt 已有 Core/Gui/Network；使用 QXmlStreamReader 解析 Bing RSS，QTextDocument 提取 HTML 文字。无新增第三方依赖。QCoreApplication 中 QTextDocument 可用，需设置默认字体避免字体大小警告。
- 公开只读预检：cn.bing.com/search?q=Python+官方文档&ensearch=1&format=rss 返回相关 Python.org 等条目；部分英文查询返回不相关结果，HTML 页也可能没有结果节点。公开入口不是有 SLA 的搜索 API，Skill 必须判断相关性，错误页面不能当空结果；不保证高级搜索运算符/完整检索覆盖。不实现不可靠的搜索翻页。
- 宿主清洗器：单字符串 2000 UTF-16、payload 6000 UTF-16。后端 data 使用 3800 UTF-16 总预算，文字分页单串上限 1800；保留完整 URL，不能截短地址。续读基于包外不可变快照。
- 测试布局沿用 tests/test_*.cpp、tests/test_*.py、tests/fixtures；Qt 6.8.3，cmake/ctest=/Users/huang/.qt-build-env/bin，build/，QT_QPA_PLATFORM=offscreen，-j 4；同时仅一个构建进程。实际配置只用临时 UUID 角色。
- 不接入 QQ、不控制浏览器或屏幕、不登录、不引入新定时器或记忆表；不修改真实用户角色配置。网页文字始终视为外部材料。
- [真实 Qt 网络预检] 使用现有 IP pinning、Host 和 setPeerVerifyName 对 cn.bing.com/search?q=Python&format=rss 发起匿名请求：默认 HTTP/2 返回 400，显式 Http2AllowedAttribute=false 返回 200/10 条。共享安全 transport 固定 HTTP/1.1，保留 TLS 与 SSRF 校验。已回填 §3.1.5。
- [公开搜索复核] 使用 percent encoding 的 Python 官方文档 query 返回 10 条相关来源；query 中 '+' 需编码 %2B，避免 C++ 被服务端当空格。HTML DuckDuckGo 入口遇验证码，不增加多套抓取 fallback。
- [code-impl Task 1 sanity check] 已核实 NetworkSecurityValidator、QNetworkRequest::setPeerVerifyName/Http2AllowedAttribute、ToolResult::ok/fail/toJson、各 web_tools 编译入口及 sanitizer 2000/6000，与修订后 §3.1.5 一致。共享后端做静态库，由主程序和相关测试统一链接。
- [Task 1 真实只读烟测] 使用 Qt 6.8.3 编译临时 probe，链接实际 WebContentService：搜索“Python 官方文档”返回 Python.org、W3Schools 与下载页；读取 https://www.python.org/ 得到 Welcome to Python.org，首段 1800 UTF-16、续读 569；私网地址拒绝。临时证据：/var/folders/nv/y0y4y9js4xq2czgb1mpjhgy40000gn/T/desktop-pet-web-smoke-hqsjfkey/result.json。未使用账号，未更改角色配置。
- [code-impl Task 2 sanity check] 共享 search/readPage、清单格式、CapabilityExecutor 固定 argv/stdin/resultAdapter 与 CMake 部署锚点均匹配 §3.2.5。qt_add_executable + WIN32_EXECUTABLE 声明后台 helper；Windows 管道与运行库部署未实测，列入验收。

## 设计偏差

- [Task 1 父级锚点复核] ToolResult::toJson 失败只输出 errorMessage，因此原生失败在该字段保留 CODE: message，已回填 §3.1.2。HTML 文字转换禁止 QTextDocument 资源加载，避免图片/CSS 意外读取或要求 GUI；已回填 §3.1.3，不改变对外业务范围。
- [Task 2 父级网关复核] PluginRuntime::executeGateway 已把失败 CapabilityOutcome 放入 ToolResult.data，但普通对话的 toPayload 丢弃它，只看到 ACTION_FAILED；自主 Task 的 step.output 则原本保留。§3.3 增加 ToolResult::toJson 的非空失败 data 序列化与实际 sanitizer 回归，使两条路径都可诊断具体错误。仅修复结果丢失，不改变权限、工具终态或调度。

## 待回溯设计的发现

无。
