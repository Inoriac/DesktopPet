# B 站内容阅读插件上下文

## 需求来源

2026-10-19 用户要求基于 https://github.com/c1overzz/dsh-bili-scroll 为项目增加视频内容阅读能力，明确启动时机交给主体已有安排，保留自主性。此前已明确暂不开发通用视觉能力。本次沿用用户持续有效的自主开发、每阶段提交授权，不重复请求例行确认。

范围：B 站搜索、视频信息、评论、带时间戳的字幕分页、可选本地语音转写、安装/登录/角色配置、现有网关与自主活动验证。不移植定时器、日报任务、评论/私信发送或 DSH 进程，不新增视觉分析。不修改用户实际角色配置或使用账号凭证。

## 决策记录

- 使用 evoloop-gen-tech-design / evoloop-code-impl；单仓库 master，起点 e795991，工作区干净；设计名 B站内容阅读-桌面端-系分.md，三个垂直 Task 串行，每 Task 独立 subagent、TDD、父级 CR、提交，最后全项目验证。用户授权覆盖例行阶段与 outline 确认，保留所有质量检查。
- 上游只读副本：/var/folders/nv/y0y4y9js4xq2czgb1mpjhgy40000gn/T/dsh-bili-scroll-review-6caun8sp，提交 5a2081bfcd67a4a3e9f24948a8449d184eb70ece，版本 0.1.10。采用其 bilibili-api-python 读取和 yt-dlp/faster-whisper 转写思路，保留两份 MIT 许可；不导入整个含写操作的 MCP 服务。
- 临时真实依赖环境：/private/tmp/desktop-pet-bilibili-dev-2rnhj2vq/.venv/bin/python（Python 3.13.14），已安装 bilibili-api-python==17.4.1、aiohttp>=3.9,<4、qrcode[pil]>=8,<9。只用于本次开发验证；项目原 Python/launcher 环境不变。系统和 CTest Python 为 3.9.6，因此纯逻辑/受控测试保持 3.9 可运行，真实网络依赖使用 3.10+。
- 已核实库签名：Video(bvid=None, aid=None, credential=None)、get_info()、get_subtitle(cid=None)、get_download_url(page_index=None,cid=None,html5=False)；search.search_by_type(keyword,search_type,order_type,page=1,page_size=42,...)；comment.get_comments(oid,type_,page_index=1,order=OrderType.TIME,credential=None)。均为异步查询。
- 已核实 QrCodeLogin.generate_qrcode()/check_state()、get_credential()/get_qrcode_picture()；登录工具为人工 CLI，不向模型公开。
- 宿主普通字符串上限 2000 UTF-16、完整 payload 上限 6000 UTF-16；command 超时由当前任务决定，通常 30 秒。通过分页控制输出，转写用有回执的后台任务，不扩大宿主上限。
- 测试布局沿用 tests/test_*.py、tests/test_*.cpp、tests/fixtures；Qt 6.8.3，cmake/ctest=/Users/huang/.qt-build-env/bin/，build/，QT_QPA_PLATFORM=offscreen，-j 4。仅一个进程操作 build/。

## 设计偏差

无。

## 待回溯设计的发现

无。
