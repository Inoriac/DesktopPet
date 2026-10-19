# 通用网页探索实现计划

系分文档：通用网页探索-桌面端-系分.md。测试布局已对照 tests/test_file_and_web_tools.cpp、test_bilibili_plugin.cpp、test_bilibili_configure.py；统一放 tests/，fixture 放 tests/fixtures/。

## Task 1：搜索与正文阅读

- 关联设计：§3.1 搜索与正文阅读
- 状态：[ ]
- Wave：1
- 依赖：无

文件：core/ai/tools/web_content_service.h/.cpp、web_tools.h/.cpp、CMakeLists.txt、tests/test_web_content.cpp、tests/test_file_and_web_tools.cpp。

测试用例：
- search_validRss_structuredResults（正常）；search_emptyRss_emptySuccess；search_invalidFeedAndNetwork_failure；search_longUnicode_budgetAndValidLinks。
- readPage_htmlAndText_readableContent（正常）；readPage_unicodePagination_losslessSnapshot；readPage_invalidOrExpiredCursor_failure；readPage_contentAndEncodingErrors_explicitFailure。
- nativeTools_sharedBackend_sameResultAndFailure；security_invalidUrlsAndArguments_rejected。

## Task 2：标准网页插件

- 关联设计：§3.2 标准网页插件
- 状态：[ ]
- Wave：2
- 依赖：Task 1

文件：plugins/web/{plugin,tool,skill}.json、USAGE.md、SKILL.md、web_command.h/.cpp、main.cpp、CMakeLists.txt、tests/test_web_command.cpp、tests/fixtures/web_command_fixture.cpp。

测试用例：
- command_status_localSuccess（正常）；command_searchAndRead_realProcessPagination（正常）；command_invalidInputAndFailure_failedEnvelope。
- command_sanitizer_preservesBudgetAndCursors；package_validSchemasAndDependencies；package_execution_digestUnchanged。

## Task 3：角色接入与自主验证

- 关联设计：§3.3 角色接入与自主验证
- 状态：[ ]
- Wave：3
- 依赖：Task 2

文件：plugins/web/configure.py、README.md、tests/test_web_configure.py、test_web_plugin.cpp、test_autonomous_chat.cpp、tests/fixtures/web_command_fixture.cpp（仅测试准备）、CMakeLists.txt、contracts/tools/README.md、插件运行时接入.md、现有工具契约适配验证.md。

测试用例：
- configure_previewApplyRepeat_preservesOtherPlugins（正常）；configure_off_removesOnlyInternalGrants；configure_invalidInputs_noWrites；configure_unicodeAndSpaces_success。
- gateway_discoverLoadInvoke_actualCommandAndPagination（正常）；gateway_failureAndAuthorization_preserved；gateway_digestUnchanged。
- autonomy_chooseWebSkill_searchReadFinish（正常）；autonomy_waitGlobalOffUnauthorized_noExecution。

## Wave 验证

每 Wave 核查共享文件顺序合并、全量测试、用例对应、测试位置、实现锚点与 context 留痕。全部完成后写审查/总结并记录真实公开网络烟测。
