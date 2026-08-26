# AI 基础框架 / 工具使用 / 权限控制 审查报告

审查范围：`core/ai`（工具运行时、权限策略引擎、文件/网络工具、MCP 适配器、Agent 调度路径）。
下面按严重程度排序，每条给出：问题 → 具体代码位置 → 可行的解决方向。

---

## 1. 文件路径白名单可被符号链接绕过（沙箱逃逸）

**问题**：`FilePathValidator` 的路径规范化只用 `QFileInfo::absoluteFilePath()` + `QDir::cleanPath()`，这两者都**不解析符号链接**，仅做字面 `.`/`..` 归并。若允许根目录内存在（或被 LLM 构造出的）指向外部的 symlink，`read_text_file` / `list_directory` / `write_text_file` 都会通过校验后跟随链接读写根目录之外的任意文件。同样的缺陷存在于 PolicyEngine 的 `normalizePolicyPath`。

**代码位置**：
- `core/ai/tools/file_tools.cpp:57-76`（`normalizePath` / `normalizeFilePath` 未用 `canonicalFilePath`）
- `core/ai/tools/file_tools.cpp:140`（读）、`:244`（列目录）、`:352`（写）都基于上述规范化做 `isPathAllowed`
- `core/ai/tools/runtime/tool_policy.cpp:6-14`（`normalizePolicyPath` 同样缺陷）

**解决方向**：读/列目录用 `QFileInfo::canonicalFilePath()` 解析真实路径后再做前缀匹配；写文件时父目录必然存在，对 `info.absoluteDir().canonicalPath()` + 文件名做校验；PolicyEngine 同步改用 canonical 解析。注意 canonical 对不存在路径返回空，需对读路径（已校验 exists）与写父目录分别处理。

---

## 2. 网络工具的 SSRF 防护未覆盖 IPv6 私有/回环节点

**问题**：`isPrivateIp` / `isReservedIp` 只调用 `QHostAddress::toIPv4Address()` 判断 IPv4 段，对纯 IPv6 地址该函数返回 0，全部判为“非私有”而放行。同时 `host == "::1"` 是字符串精确匹配，无法拦截 `0:0:0:0:0:0:0:1` 等非规范写法。结果：LLM 可通过 `http://[fc00::1]`、`http://[fe80::...]`、非规范回环等形式访问内网 IPv6 服务 / 云元数据。

**代码位置**：
- `core/ai/tools/web_tools.cpp:83-125`（`isPrivateIp` / `isReservedIp` 仅 IPv4）
- `core/ai/tools/web_tools.cpp:45`（仅 `"::1"` 字符串匹配）

**解决方向**：用 `addr.protocol() == QAbstractSocket::IPv6Protocol` 分支处理：拒绝 `::1`（回环）、`fe80::/10`（link-local）、`fc00::/7`（ULA），以及对 `::ffff:x.x.x.x` IPv4-mapped 地址先还原 IPv4 再走 IPv4 检查；或直接用一个完整的 CIDR 黑名单匹配器统一处理 v4/v6。

---

## 3. L3 “需用户确认” 权限闸门（已解决）

**当前状态**：`ToolRuntime` 会暂存待确认请求，`AIBrain` 将确认请求交给 UI，并通过同一 requestId 调用 `resolveConfirmation`。批准后以已确认状态执行，拒绝后返回受控失败；运行时切换或停止时会清理待确认请求。

**代码位置**：
- `core/ai/tools/runtime/tool_runtime.cpp`（`resolveConfirmation` / `executeImpl`）
- `core/ai/ai_brain_router.cpp`、`core/ai/ai_brain_loop.cpp`（确认 continuation）
- `ui/petwindow.cpp`（用户确认 UI）

**结论**：该问题已由 `AIBrain` 单主链路闭环，不再保留第二套 Agent 确认实现。

---

## 4. URL 校验与实际请求之间存在 TOCTOU / DNS Rebinding

**问题**：`isUrlAllowed` 先用 `QHostInfo::fromName` 解析主机名做内网判定，随后 `manager.get(request)` 会**再次独立解析 DNS**。攻击者控制的 DNS 可在校验时返回公网 IP、在真正请求时返回内网 IP，从而绕过 SSRF 检查访问内网服务。

**代码位置**：
- `core/ai/tools/web_tools.cpp:57-73`（`isIpBlocked` → `resolveHost` 仅用于校验）
- `core/ai/tools/web_tools.cpp:186`（`manager.get` 二次解析）

**解决方向**：校验通过后用解析到的 IP 直接发起连接（`QNetworkRequest` 指向 IP 并手工设置 `Host` 头），或在请求上绑定已校验的地址；同时显式关闭 HTTP 重定向跟随（`QNetworkRequest::FollowRedirectAttribute = false`），避免重定向到未校验的内网 URL。

---

## 5. 动态外部 MCP 工具默认按工具名子串推断为低风险并自动放行

**问题**：`McpToolAdapter` 把所有外部 MCP 工具注册为 `ToolCategory::Action`，`PolicyEngine::inferRiskLevel` 仅按名字是否包含 `delete/shell/write/read` 等子串判级；名字“无害”的外部工具（如 `send_email`、`db_query`、`run_job`——`run` 不在 `run_process` 子串内）会落到 L2 **自动放行**，无需确认。外部动态工具的能力是任意的，不应靠名字推断默认信任。

**代码位置**：
- `core/ai/mcp/mcp_tool_adapter.cpp:5-10`（构造时固定 `ToolCategory::Action`）
- `core/ai/tools/runtime/tool_policy.cpp:142-157`（`inferRiskLevel` 仅按名字子串）

**解决方向**：动态注册的工具（尤其 MCP / 第三方）默认应为 L3 `RequireConfirmation`，或显式 `Deny` 直到在配置中按工具名逐一声明风险等级；`PolicyEngine` 对“未分类外部工具”取保守默认，而非回退到 L2。

---

## 6. `execute_whitelisted_command` 的参数过滤器在非 shell 场景过度拦截合法参数

**问题**：命令通过 `QProcess::setArguments` 逐参数传入，**不经过 shell**，因此 `;`、`|`、`>`、`<`、`&&` 等都不是元字符。但 `areArgumentsSafe` 仍把包含这些 token 的参数判定为“可疑 shell 片段”拒绝；同时 `isPathLikeArgument` 把任何含 `/` 或以 `.` 开头的参数都当路径并强制必须在 `allowedRoots` 内，于是 URL、日期 `2026/07/29`、正则、相对引用等合法参数被误拒。这让白名单命令工具在多数真实调用下不可用。

**代码位置**：
- `core/ai/tools/file_tools.cpp:545-588`（`areArgumentsSafe` 的 suspiciousTokens 与 controlChars 检查）
- `core/ai/tools/file_tools.cpp:590-606`（`isPathLikeArgument` 启发式过宽）

**解决方向**：既然不走 shell，删除 suspiciousTokens 这一层（保留长度/控制字符上限即可）；将“路径型参数”检测收紧为“以 `/` 开头或在 cwd 下解析为已存在文件”的明确路径，或改为仅在白名单命令声明了“接受路径参数”时才校验。

---

## 7. `grantedToolNames`（自动授权）无差别覆盖确认闸门，包括 L3

**问题**：`PolicyEngine::evaluate` 在风险分级 switch **之前**就判断 `grantedToolNames.contains(tool.name()) → Allow`，因此只要某工具被配置进 `autoGrantedTools`，无论它是 L2 还是 L3 都会跳过确认直接执行。默认 `autoGrantedTools` 只有读工具，但该机制允许配置方把 `write_text_file` 等加入后静默失去确认；叠加默认 `allowedRoots = 当前工作目录`，配置失误即把高风险工具变成无确认可执行。

**代码位置**：
- `core/ai/tools/runtime/tool_policy.cpp:120-122`（grant 命中即 Allow，先于风险 switch）
- `core/configLoader/config_manager.cpp:255-258`、`ui/petwindow_ai.cpp` 注入 `autoGrantedTools`

**解决方向**：令 grant 仅对 L0/L1 生效；L3 即使被 grant 也至少降级为“本轮允许、仍记审计”而非完全免确认；或明确 `autoGrantedTools` 只接受读类工具并在加载配置时校验。

---

## 附：次要观察

- **`web_fetch` 描述与实现不符**：`web_tools.cpp:134-137` 的描述声称“返回纯文本内容，不包含 HTML 标签”，但 `:217-221` 实际 `QString::fromUtf8(data)` 原样返回 HTML。应要么改描述，要么在返回前剥 HTML。
- **PolicyEngine 中 L1 空 roots 分支不可达**：`tool_policy.cpp:128-131` 对本地文件工具在 `allowedRootPaths` 为空时希望 `requireConfirmation`，但 `scopedArgumentsAllowed`（`:203-206`）已先以 Deny 终止；同样 `evaluate` 中 `L4` 的 switch case（`:136-137`）因上方提前 Deny 而为死代码。两处建议统一：空 roots 究竟是 Deny 还是 RequireConfirmation 应只保留一处。
- **结果脱敏按 key 子串匹配**：`tool_result_sanitizer.cpp:104-110` `isSensitiveKey` 对含 `token` 的键整段 `[REDACTED]`，会误伤 `tokenizer`、`tokens_used` 等统计字段；可收紧为词边界匹配或白名单放行统计型字段。
