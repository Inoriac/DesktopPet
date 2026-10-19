# 通用网页探索实现总结

## 交付

- §3.1：WebContentService 统一实现公开 Bing RSS 搜索和 HTML/纯文本阅读；原生工具复用后端。正文使用包外快照按游标续读，控制输出预算并保留完整 Unicode 与来源。网络失败、错误内容和无结果分别处理。
- §3.2：plugins/web 提供标准 command 的 status/search/read_page 与按需加载的 Skill。主体通过原有网关和活动机制决定搜索、继续、结束或等待，无新调度器。
- §3.3：独立标准库 configure.py 为指定角色预览/应用配置、备份与合并授权；ToolResult 失败时保留已有非空 data，使插件具体错误进入普通模型上下文。README、验证备注和契约入口齐全。

入口：[使用说明](../../../plugins/web/README.md)、[验证备注](../../../plugins/web/验证备注.md)、[代码审查](./通用网页探索-桌面端-审查.md)。进度唯一来源为同目录 impl-plan。

## 主要文件

core/ai/tools/web_content_service.h/.cpp、web_tools.h/.cpp；plugins/web/；include/ai_types.h；CMakeLists.txt；tests/test_web_content.cpp、test_web_command.cpp、test_file_and_web_tools.cpp、fixtures/web_command_fixture.cpp；contracts/tools 的索引、运行时接入与适配状态文档。

## 实际验证与延期

共享后端阶段全项目构建与 72/72 CTest 通过（40.62 秒）。实际 Qt 6.8.3 共享库完成匿名搜索“Python 官方文档”、读取 Python 官网正文和续读，私网地址被拒绝。

用户随后明确要求测试留待自己执行。后续仅完成必要编译与代码审查：desktop_pet_web、web_command_fixture、web_command_tests、最终 Desktop_Pet 均编译成功；configure.py AST 语法检查通过。最终 command/schema/角色配置/网关/自主/Windows 验收未运行，详见验证备注。没有配置真实角色、使用账号或新增第三方依赖。

## TDD 与设计调整

Task 1 完成 RED→GREEN；Task 2 在接口桩 RED 后收到测试延期要求，最终测试未执行；Task 3 按该要求以编译和完整验收备注交付，没有新增专用网页网关/自主自动化测试。

父级真实预检发现 Qt HTTP/2 与 IP pinning 的 authority 不兼容，安全请求改用 HTTP/1.1；网关失败 data 在普通对话序列化时丢失，补上非空失败数据输出。两项均已回填设计和 context，不改变授权或活动调度。原生 results 数组与分页 content 的兼容变化已写入使用说明。
