# Windows 新插件验收（2026-10-20）

基线 `8cab2b3`，范围为 `48c3e80..8cab2b3` 的 Web、Bilibili、按需插件上下文与自主候选更新。Windows 11 / Qt 6.5.3 / MinGW 11.2 / Python 3.12。保留此前未提交的记忆修复及用户文件。

验收依据：`plugins/web/验证备注.md`、`plugins/bilibili/README.md`、`.evo/tasks/plugin-context-loading/Task2-验证记录.md`。证据位于 `build/acceptance-2026-10-20/`（忽略目录）。以下区分真实外网、真实模型与受控测试，不以进程退出 0 代替业务成功。

## 结论

- Web 搜索、HTTPS 正文、完整分页和独立 Windows 部署可用；发现并修复了含代码标记网页导致 helper 崩溃的问题。
- B 站匿名搜索、第二 P 信息与评论成功；登录字幕及真实 ASR 尚不能签收。
- **真实模型的标准插件端到端不能整体签收**：`gemini-3.8-flash-high` 可以响应，但多轮 `invoke_capability` 的业务参数为空，遭正确拒绝；不是模型名调用不通，也不是 Web helper 不联网。
- 原生 `web_fetch` 可以读正文并如实解释 DNS 失败，但自然语言跨轮续读未保住游标；不以原生工具成功冒充标准插件成功。

## 自动化与补测

- 完整构建成功；Windows CTest 首次全量 **73/73**，52.82 秒；最终改动后再次 **73/73**，51.05 秒，见 `ctest-final.log`。
- 通用契约 **96/96**，`contracts.json`。
- Bilibili Adapter / Transcription / Setup / Configure 四组 Python 测试通过。
- 插件上下文、候选轮转、聊天投影、任务隔离、Bilibili 网关和自主 read/wait/全局关闭均有本机受控测试通过；不等同于真实模型自主完成阅读。
- 新增 `WebCommandTests`：代码标记正文崩溃回归；真实 PluginRuntime 网关加载与读取、失败详情、未加载/外部会话/禁用拒绝；有效 URL 参数校验；失败 payload 中错误保留且假 api_key/token/password 递归脱敏，普通无 data 失败兼容。

## Web 真实命令与部署

- `status` completed；未创建缓存。PE subsystem=2（GUI），helper 不分配控制台；未做肉眼焦点/闪窗观察。
- UTF-8 输入“Python 官方文档”：回显 query 完整，3 条结果为 Python 官网、W3Schools、Python 下载页。相关于 Python，但不是 3 条官方文档结果；搜索质量只是部分满足意图。
- `https://www.python.org/` 正文与所有后续页成功，按原 URL 和游标拼接后与缓存完整正文精确一致。新首读不破坏旧游标；换 URL、过期快照及损坏游标返回 `INVALID_CURSOR`。
- 空搜索返回 `INVALID_ARGUMENT`；127.0.0.1、带用户密码 URL、file URL 返回 `URL_NOT_ALLOWED`；404 返回 `HTTP_ERROR`；`.invalid` 域返回 `NETWORK_ERROR`；缓存路径为文件返回 `CACHE_ERROR`。
- 独立复制到 `网页 程序/desktop_pet_web.exe`，用同套 windeployqt 部署，PATH 仅保留 Windows System32 后，中文搜索和 Python 教程 HTTPS 读取通过。搜索曾出现一次 DNS 失败，重试正常。
- HTML 提取会保留少量导航；教程正文可读，不能声称具备浏览器脚本渲染能力。
- 离线续页的“不重新请求后端”、Unicode 边界、64 快照上限、损坏编码/PDF 拒绝由受控测试验证；没有断开整台电脑网络，也没有批量抓取 64 个真实页面。

证据：`web-live.jsonl`、`web-live.log`、`web-deployed.jsonl`、`web-gateway-tests.log`。

### 本次修复

生产 helper 读取 `https://docs.python.org/3/tutorial/` 稳定退出 `0xC0000005`。GDB 栈位于 `QTextDocument::setHtml` / `QTextHtmlParser`。最小 `<code>print()</code>` 子进程测试同样失败。

原因：`QCoreApplication` 不提供该 HTML 路径使用的 GUI 字体/调色板服务。生产入口和命令 fixture 改用 `QGuiApplication`，设置 `AA_PluginApplication`，不创建窗口。无需更改用户配置。Windows 正常平台插件由 windeployqt 部署；离屏测试使用现有 Qt offscreen 测试环境。

修复后最小回归通过，同一真实教程页面返回正文、完整 URL 和 nextCursor；独立中文路径部署也通过。证据 `gdb-web.log`、`web-red.log`、`web-fixed-tests2.log`。

## 配置助手

隔离 UUID 目录、中文/空格缓存路径：预览不写配置或缓存；首次应用 8 条授权；重复应用 changed=false 且不新增备份；保留无关字段/其他插件；autonomous off 仅保留本地主人 4 条授权，on 恢复 8 条。

无效 UUID、相对路径、缺失 helper、包内缓存及损坏 JSON 均拒绝且不覆盖原文件。配置验证脚本 `config_check.py`。运行没有在插件包中留下缓存或 `__pycache__`。备份生成已检查，未进行用户正式角色的备份恢复。

## 真实模型行为

使用用户授权的 `gemini-3.8-flash-high`；返回 model 为服务端的 `gemini-3.8-flash-n`。正式配置不变。隔离角色关闭声音/屏幕功能，初始全局自主关闭，随后另行短时观察自主。初次 45 秒模型请求超时，测试副本放宽到 120 秒；期间也观察到 Connection closed。

1. 标准插件发现、按需加载及一次搜索确实完成；多次读取请求因业务参数校验失败，最终达到工具轮数上限。不是授权绕过，失败时没有执行 helper。
2. 响应日志刻意将工具参数脱敏成 `{}`，单看该日志不能断言真实参数为空。本次追加了临时定点诊断，确认失败的执行器实际业务参数也为 `{}`；临时诊断已移除。
3. 对服务端做独立最小 SSE 请求：普通 `report(city)` 能返回 Shanghai；保持宿主原始 `invoke_capability` schema 并明确要求 arguments.url 时，原始 SSE 仍返回 `"arguments":{}`，ref 正确。证据 `provider-gateway-probe.sse`。说明该模型/服务对当前无属性业务对象 schema 的配合存在实际问题，不能仅靠“再提示一次”当作修复。
   对照请求仅将 arguments schema 改成显式包含必填 url，原始 SSE 随即返回正确 URL（`provider-typed-probe.sse`）。该对照没有改正式网关。后续应评估按已加载操作投影业务 schema，并保持宿主权限与校验；不能用放松校验或猜填参数解决。
4. 干净会话的正文摘要通过的是原生 `web_fetch`；下一轮要求按旧游标续读时，模型称没有保存有效 nextCursor，并错误否认上轮实际读取。记为跨轮游标/工具历史可用性问题，未修复。
5. 原生工具 DNS 失败后模型明确报告 `NETWORK_ERROR / 无法解析主机`；标准插件的失败详情保留另由真实网关受控测试确认。
6. 包被修改后，旧宿主返回 `STALE_REVISION`；重启后可重新发现加载，拒绝过期定义符合预期。

证据 `host-dialogues.jsonl`、`host-*.log`、`provider-*-probe.sse`。本轮未修改通用网关 schema、调高正式工具轮数或换模型来掩盖失败。

## Bilibili 真实访问与环境限制

- 本机 pip 默认镜像找不到固定依赖；改用官方 pypi 源后基础 setup 成功。仅创建验收专用 venv，没有改全局 pip 配置。
- 初次中文搜索 15 秒超时；隔离设置延长至 25 秒后 `Python` 搜索成功，选取真实结果 `BV1rpWjevEip`。
- 第二 P 信息返回 `part=2`、`cid=500001658674168` 和 `?p=2` 链接；评论成功。
- 第二 P 字幕明确返回 `AUTH_REQUIRED`；未登录，因此没有假定字幕内容或代用户扫码。
- ASR 未开启时返回 `ASR_DISABLED`。显式准备 tiny 的依赖下载第一次超时，延长 pip 下载超时后依赖安装成功，但 Hugging Face 模型下载因 TLS `UNEXPECTED_EOF_WHILE_READING` 失败。没有完整模型，真实引擎/平台音频下载链未验收；受控后台转写测试通过不能替代它。
- 独立平台 HTTP 探测曾遇到 412，平台可用性有波动。

证据 `bili-setup*.log`、`bili-live.jsonl`、`bili-retry.json`、`bili-more.log`、`bili-asr-retry.log`。

## 自主观察与清理

初始全局自主关闭时没有创建自主活动。随后隔离配置设为 2 秒 tick、20 秒最小选择间隔、每小时最多 4 次选择，观察超过 148 秒。持久化预算记录显示 4 次真实模型选择均已结算，最后结果为 `failure / MODEL_OUTPUT_INVALID`，无自主活动创建。达到预算后未继续选择，不能将这个结果记成合法 wait 或成功阅读。

关闭全局自主并将 Web 配置为 autonomous off 后，超过 69 秒仍保持 4 次历史选择，没有新增自主选择。该阶段的显式主人 status 请求仍陷入模型错误调用/参数校验重试，不能宣称真实模型主人调用已经稳定；配置保留主人授权以及网关权限边界由受控测试验证。

证据 `autonomy-observation.json`、`autonomy-runtime-state.json`。只操作隔离角色，不改变正式角色的全局自主开关或插件授权。验收结果不包含未实际完成的长期自主稳定性、登录字幕、ASR 中文准确率或肉眼窗口焦点检查。

已停止验收进程、移除临时角色注册，删除含密钥的 launch 副本和会话 token，保留隔离数据库与脱敏证据（`cleanup.json`）。正式角色、正式持久化模型配置和此前记忆修复均保留。本报告随 Web 修复及回归测试一同提交；原始本机日志和隔离数据库不纳入 Git。
