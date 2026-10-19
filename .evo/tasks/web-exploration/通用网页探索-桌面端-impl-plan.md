# 通用网页探索实现计划

系分文档：通用网页探索-桌面端-系分.md。测试布局已对照 tests/test_file_and_web_tools.cpp、test_bilibili_plugin.cpp、test_bilibili_configure.py；统一放 tests/，fixture 放 tests/fixtures/。

用户追加安排：测试延后自行验收。Task 1 已执行结果保留；Task 2/3 后续不再扩大测试，完成实现、CR 和必要编译，测试用例清单作为 plugins/web/验证备注.md 的验收来源。已写测试源码保留，未新增测试可用备注交付，不将未运行项目标为通过。

## Task 1：搜索与正文阅读

- 关联设计：§3.1 搜索与正文阅读
- 状态：[x]
- Wave：1
- 依赖：无

文件：core/ai/tools/web_content_service.h/.cpp、web_tools.h/.cpp、CMakeLists.txt、tests/test_web_content.cpp、tests/test_file_and_web_tools.cpp。

测试用例：
- search_validRss_structuredResults（正常）；search_emptyRss_emptySuccess；search_invalidFeedAndNetwork_failure；search_longUnicode_budgetAndValidLinks。
- readPage_htmlAndText_readableContent（正常）；readPage_unicodePagination_losslessSnapshot；readPage_invalidOrExpiredCursor_failure；readPage_contentAndEncodingErrors_explicitFailure。
- nativeTools_sharedBackend_sameResultAndFailure；security_invalidUrlsAndArguments_rejected。

## Task 2：标准网页插件

- 关联设计：§3.2 标准网页插件
- 状态：[x]
- Wave：2
- 依赖：Task 1

文件：plugins/web/{plugin,tool,skill}.json、USAGE.md、SKILL.md、web_command.h/.cpp、main.cpp、CMakeLists.txt、tests/test_web_command.cpp、tests/fixtures/web_command_fixture.cpp。

测试用例：
- command_status_localSuccess（正常）；command_searchAndRead_realProcessPagination（正常）；command_invalidInputAndFailure_failedEnvelope。
- command_sanitizer_preservesBudgetAndCursors；package_validSchemasAndDependencies；package_execution_digestUnchanged。

## Task 3：角色接入与自主验证

- 关联设计：§3.3 角色接入与自主验证
- 状态：[x]
- Wave：3
- 依赖：Task 2

文件：plugins/web/configure.py、README.md、验证备注.md、tests/test_web_configure.py、test_web_plugin.cpp、test_autonomous_chat.cpp、tests/fixtures/web_command_fixture.cpp（仅测试准备）、include/ai_types.h、tests/test_file_and_web_tools.cpp、CMakeLists.txt、contracts/tools/README.md、插件运行时接入.md、现有工具契约适配验证.md。测试源新增按用户延期安排可改为详细验收备注。

测试用例：
- configure_previewApplyRepeat_preservesOtherPlugins（正常）；configure_off_removesOnlyInternalGrants；configure_invalidInputs_noWrites；configure_unicodeAndSpaces_success。
- gateway_discoverLoadInvoke_actualCommandAndPagination（正常）；gateway_failureAndAuthorization_preserved；gateway_digestUnchanged。
- result_failedPayload_preservesDetailsAndRedactsSensitiveKeys；result_emptyFailure_unchanged。
- autonomy_chooseWebSkill_searchReadFinish（正常）；autonomy_waitGlobalOffUnauthorized_noExecution。

## Wave 验证

每 Wave 核查共享文件顺序合并、实现锚点与 context 留痕，执行 CR 和必要编译。后续完整测试按用户要求延期，现有测试位置与待测用例保留记录。全部实现后写审查/总结，明确实际验证边界。

- Wave 1：串行文件修改无冲突；全项目构建成功、72/72 CTest 通过；§3.1 场景均有对应测试，位置遵循 tests/test_*.cpp；父级审查与实现锚点留痕齐全，真实公开搜索/正文/续读烟测通过。
- Wave 2：实现与父级 CR 完成；desktop_pet_web、web_command_fixture、web_command_tests 编译成功。用户在测试桩 RED 后要求延后测试，因此最终行为测试/schema/Windows 验证未执行，不宣称 GREEN；保留测试源码并转入验收备注。串行无冲突，测试位置与锚点留痕齐全。
- Wave 3：配置、失败结果处理、使用说明与验证备注完成并经父级 CR；configure.py AST 检查、Desktop_Pet 链接及 web_command_tests 编译通过，git diff --check 通过。按用户安排未运行行为测试、网关/自主验证或外网烟测；没有修改真实角色配置。专用网关/自主用例改为完整手工验收步骤，现有测试命令和未测范围已写明。三 Task 实现与修订后设计一致，无待回溯项。
