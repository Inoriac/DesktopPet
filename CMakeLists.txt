cmake_minimum_required(VERSION 4.0)
project(Desktop_Pet)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 添加编译优化选项
if(CMAKE_BUILD_TYPE MATCHES Release)
    if(MSVC)
        add_compile_options(/O2 /GL)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /LTCG")
    else()
        add_compile_options(-O3 -DNDEBUG)
    endif()
    message(STATUS "Building in Release mode with optimizations")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_PREFIX_PATH "E:/Qt/6.5.3/mingw_64/lib/cmake")

find_package(Qt6 COMPONENTS
        Core
        Gui
        Widgets
    Network
        OpenGL
        OpenGLWidgets
        Sql
        Test
        REQUIRED)

# 收集所有源文件
set(UI_SOURCES
    ui/mainwindow.cpp
    ui/mainwindow.h
    ui/animated_combo_box.cpp
    ui/animated_combo_box.h
    ui/animated_scrollbar.cpp
    ui/animated_scrollbar.h
    ui/card_widget.cpp
    ui/card_widget.h
    ui/navigation_widget.cpp
    ui/navigation_widget.h
    ui/round_slider.cpp
    ui/round_slider.h
    ui/switch_button.cpp
    ui/switch_button.h
    ui/theme_manager.cpp
    ui/theme_manager.h
    ui/chat_history_window.cpp
    ui/chat_history_window.h
    ui/liquidglasschatbubble.cpp
    ui/liquidglasschatbubble.h
    ui/liquidglasschatbubble_utils.cpp
    ui/liquidglassmaterial.cpp
    ui/liquidglassmaterial.h
    ui/petwindow.cpp
    ui/petwindow_interaction.cpp
    ui/petwindow_ai.cpp
    ui/petwindow_screen_chat.cpp
    ui/petwindow_bubble.cpp
    ui/petwindow_native.cpp
    ui/petwindow_snap.cpp
    ui/petwindow.h
    ui/trayicon.cpp
    ui/trayicon.h
)

set(CORE_SOURCES
    core/animation/animation_manager.cpp
    core/animation/animation_manager.h
    core/animation/animation_importer.cpp
    core/animation/animation_importer.h

    core/behavior/behavior_manager.cpp
    core/behavior/behavior_manager.h

    core/configLoader/config_manager.cpp
    core/configLoader/config_manager.h

    core/eventHandler/event_handler.cpp
    core/eventHandler/event_handler.h

    core/controller/pet_controller.cpp
    core/controller/pet_controller.h

    core/reminder/pet_reminder_manager.cpp
    core/reminder/pet_reminder_manager.h

    core/voice/voice_synthesis_service.cpp
    core/voice/voice_synthesis_service.h

    core/animation/animation_player.cpp
    core/animation/animation_player.h
    core/animation/animation_crossfader.cpp
    core/animation/animation_crossfader.h
    core/ai/llm/llm_client.h
    core/ai/llm/openai_compatible_client.h
    core/ai/llm/openai_compatible_client.cpp
    core/ai/llm/llm_chat_service.h
    core/ai/llm/llm_chat_service.cpp

    core/ai/context_builder.h
    core/ai/context_builder.cpp
    core/ai/ai_call_logger.h
    core/ai/ai_call_logger.cpp
    core/ai/ai_brain.h
    core/ai/ai_brain.cpp
    core/ai/ai_brain_router.cpp
    core/ai/ai_brain_loop.cpp

    core/ai/ai_tool.h
    core/ai/tool_registry.cpp
    core/ai/tool_registry.h
    core/ai/agent/agent_state.h
    core/ai/agent/agent_session.h
    core/ai/agent/agent_session.cpp
    core/ai/agent/agent_core.h
    core/ai/agent/agent_core.cpp
    core/ai/router/intent_types.h
    core/ai/router/intent_router.h
    core/ai/router/intent_router.cpp
    core/ai/scheduler/scheduled_task.h
    core/ai/scheduler/scheduled_task.cpp
    core/ai/scheduler/agent_scheduler.h
    core/ai/scheduler/agent_scheduler.cpp
    core/ai/tools/runtime/tool_policy.h
    core/ai/tools/runtime/tool_policy.cpp
    core/ai/tools/runtime/tool_result_sanitizer.h
    core/ai/tools/runtime/tool_result_sanitizer.cpp
    core/ai/tools/runtime/tool_runtime.h
    core/ai/tools/runtime/tool_runtime.cpp
    core/ai/context/context_budget.h
    core/ai/context/context_manager.h
    core/ai/context/context_manager.cpp
    core/ai/memory/memory_types.h
    core/ai/memory/memory_extractor.h
    core/ai/memory/memory_extractor.cpp
    core/ai/memory/memory_policy.h
    core/ai/memory/memory_policy.cpp
    core/ai/memory/memory_retriever.h
    core/ai/memory/memory_retriever.cpp
    core/ai/memory/memory_store.h
    core/ai/memory/memory_store.cpp
    core/ai/memory/memory_repository.h
    core/ai/memory/sqlite_memory_repository.h
    core/ai/memory/sqlite_memory_repository.cpp
    core/ai/memory/memory_json_codec.h
    core/ai/memory/memory_json_codec.cpp
    core/ai/memory/memory_relation.h
    core/ai/memory/memory_relation_graph.h
    core/ai/memory/memory_relation_graph.cpp
    core/ai/memory/working_memory_cache.h
    core/ai/memory/working_memory_cache.cpp
    core/ai/memory/embedding_provider.h
    core/ai/memory/embedding_index.h
    core/ai/memory/noop_embedding_index.h
    core/ai/mcp/mcp_client.h
    core/ai/mcp/mcp_client.cpp
    core/ai/mcp/mcp_server_process.h
    core/ai/mcp/mcp_server_process.cpp
    core/ai/mcp/mcp_tool_adapter.h
    core/ai/mcp/mcp_tool_adapter.cpp
    core/ai/tools/animation_tools.h
    core/ai/tools/companion_tools.h
    core/ai/tools/companion_tools.cpp
    core/ai/tools/environment_tools.h
    core/ai/tools/environment_tools.cpp
    core/ai/tools/life_tools.h
    core/ai/tools/life_tools.cpp
    core/ai/tools/memory_tools.h
    core/ai/tools/memory_tools.cpp
    core/ai/tools/music_tools.h
    core/ai/tools/schedule_tools.h
    core/ai/tools/schedule_tools.cpp
    core/ai/tools/file_tools.h
    core/ai/tools/file_tools.cpp
    core/ai/tools/web_tools.h
    core/ai/tools/web_tools.cpp
    core/ai/skill/skill_types.h
    core/ai/skill/skill_types.cpp
    core/ai/skill/skill_store.h
    core/ai/skill/skill_store.cpp
    core/ai/skill/skill_matcher.h
    core/ai/skill/skill_matcher.cpp
    core/ai/skill/skill_tools.h
    core/ai/skill/skill_tools.cpp
)

set(ENGINE_SOURCES
    engine/model_loader.cpp
    engine/model_loader_geometry.cpp
    engine/model_loader_skeleton.cpp
    engine/model_loader.h
    engine/platform_utils.cpp
    engine/platform_utils.h
    engine/render_engine.cpp
    engine/render_engine_resources.cpp
    engine/render_engine_interaction.cpp
    engine/render_engine.h
    engine/shader_manager.cpp
    engine/shader_manager.h
    engine/sound_engine.cpp
    engine/sound_engine.h
    engine/render_viewport.cpp
    engine/render_viewport.h
)

set(INCLUDE_SOURCES
    include/common.h
    include/global_types.h
    include/macros.h
    include/animation_types.h
    include/pet_reminder_types.h
    include/material_types.h
    include/model_types.h
    include/render_types.h
        include/ai_types.h
)

set(TINYGLTF_SOURCES
        third_party/tinygltf/tiny_gltf.cc
        third_party/tinygltf/json.hpp
        third_party/tinygltf/stb_image.h
)

set(ENTITY_SOURCES
        entity/pet.cpp
        entity/pet.h
        entity/pet_personality.cpp
        entity/pet_personality.h
)

set(STATISTIC_SOURCES
        statistic/statistic_manager.h
        statistic/statistic_manager.cpp
        include/statistic_types.h
)

set(MUSIC_SOURCES

)

# 合并所有源文件
set(ALL_SOURCES
    main.cpp
    ${UI_SOURCES}
    ${CORE_SOURCES}
    ${ENGINE_SOURCES}
    ${INCLUDE_SOURCES}
    ${TINYGLTF_SOURCES}
    ${ENTITY_SOURCES}
    ${STATISTIC_SOURCES}
    ${MUSIC_SOURCES}
)

add_executable(Desktop_Pet ${ALL_SOURCES})

# 设置包含目录
target_include_directories(Desktop_Pet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinygltf
    ${CMAKE_CURRENT_SOURCE_DIR}/entity
    ${CMAKE_CURRENT_SOURCE_DIR}/statistic
    ${CMAKE_CURRENT_SOURCE_DIR}/music
)

target_link_libraries(Desktop_Pet
        Qt::Core
        Qt::Gui
        Qt::Widgets
    Qt::Network
        Qt::OpenGL
        Qt::OpenGLWidgets
        Qt::Sql
        psapi
)

set(TEST_SOURCES
#        tests/test_statistic_manager.cpp
#        tests/test_pet_reminder_manager.cpp
#        tests/test_animation_importer.cpp
        tests/test_animation_player.cpp
)

add_executable(statistic_tests ${TEST_SOURCES}
                               core/animation/animation_player.cpp
                               core/animation/animation_player.h
                               core/animation/animation_crossfader.cpp
                               core/animation/animation_crossfader.h
                               core/animation/animation_importer.cpp
                               core/animation/animation_importer.h
                               engine/model_loader.cpp
                               engine/model_loader_geometry.cpp
                               engine/model_loader_skeleton.cpp
                               engine/model_loader.h
                               ${TINYGLTF_SOURCES}

)

target_include_directories(statistic_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/entity
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinygltf
)

target_link_libraries(statistic_tests
        Qt::Core
        Qt::Test
        Qt::Widgets
    Qt::Network
        Qt::OpenGL
        Qt::OpenGLWidgets
        Qt::Sql
        psapi
)

enable_testing()
add_test(NAME StatisticTests COMMAND statistic_tests)

# ---- Tool 系统测试 ----
# 轻量测试，不依赖 OpenGL / 模型文件
add_executable(tool_tests
        tests/test_tool_registry.cpp
        core/ai/tool_registry.cpp
        core/ai/tool_registry.h
        core/ai/ai_tool.h
        core/ai/tools/runtime/tool_policy.h
        core/ai/tools/runtime/tool_policy.cpp
        core/ai/tools/runtime/tool_result_sanitizer.h
        core/ai/tools/runtime/tool_result_sanitizer.cpp
        core/ai/tools/runtime/tool_runtime.h
        core/ai/tools/runtime/tool_runtime.cpp
        core/ai/tools/environment_tools.h
        core/ai/tools/environment_tools.cpp
        core/ai/tools/animation_tools.h
        include/ai_types.h
        ${STATISTIC_SOURCES}
)

target_include_directories(tool_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai/tools
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
        ${CMAKE_CURRENT_SOURCE_DIR}/entity
)

target_link_libraries(tool_tests
        Qt::Core
    Qt::Network
        Qt::Test
)

add_test(NAME ToolTests COMMAND tool_tests)

# ---- Intent 路由测试 ----
add_executable(intent_router_tests
        tests/test_intent_router.cpp
        core/ai/router/intent_router.h
        core/ai/router/intent_router.cpp
        core/ai/router/intent_types.h
)

target_include_directories(intent_router_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
)

target_link_libraries(intent_router_tests
        Qt::Core
        Qt::Test
)

add_test(NAME IntentRouterTests COMMAND intent_router_tests)

# ---- Memory 写入策略测试 ----
add_executable(memory_strategy_tests
    tests/test_memory_strategy.cpp
    core/ai/memory/memory_types.h
    core/ai/memory/memory_extractor.h
    core/ai/memory/memory_extractor.cpp
    core/ai/memory/memory_policy.h
    core/ai/memory/memory_policy.cpp
    core/ai/memory/memory_retriever.h
    core/ai/memory/memory_retriever.cpp
    core/ai/memory/memory_store.h
    core/ai/memory/memory_store.cpp
    core/ai/memory/memory_repository.h
    core/ai/memory/sqlite_memory_repository.h
    core/ai/memory/sqlite_memory_repository.cpp
    core/ai/memory/memory_json_codec.h
    core/ai/memory/memory_json_codec.cpp
    core/ai/memory/memory_relation.h
    core/ai/memory/memory_relation_graph.h
    core/ai/memory/memory_relation_graph.cpp
    core/ai/memory/working_memory_cache.h
    core/ai/memory/working_memory_cache.cpp
    core/ai/tools/memory_tools.h
    core/ai/tools/memory_tools.cpp
    core/ai/memory/embedding_provider.h
    core/ai/memory/embedding_index.h
    core/ai/memory/noop_embedding_index.h
)

target_include_directories(memory_strategy_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
)

target_link_libraries(memory_strategy_tests
    Qt::Core
    Qt::Sql
    Qt::Test
)

add_test(NAME MemoryStrategyTests COMMAND memory_strategy_tests)

# ---- LLM 异步服务测试 ----
add_executable(llm_tests
    tests/test_llm_chat_service.cpp
    core/ai/llm/llm_chat_service.h
    core/ai/llm/llm_chat_service.cpp
    core/ai/llm/llm_client.h
    core/ai/llm/openai_compatible_client.h
    core/ai/llm/openai_compatible_client.cpp
    core/configLoader/config_manager.h
    core/configLoader/config_manager.cpp
    statistic/statistic_manager.h
    statistic/statistic_manager.cpp
    include/statistic_types.h
    include/ai_types.h
    include/global_types.h
)

target_include_directories(llm_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
    ${CMAKE_CURRENT_SOURCE_DIR}/core/ai/llm
    ${CMAKE_CURRENT_SOURCE_DIR}/statistic
)

target_link_libraries(llm_tests
    Qt::Core
    Qt::Gui
    Qt::Network
    Qt::Test
)

add_test(NAME LlmTests COMMAND llm_tests)

# ---- 文件和网络工具测试 ----
add_executable(file_web_tool_tests
    tests/test_file_and_web_tools.cpp
    core/ai/tools/file_tools.cpp
    core/ai/tools/web_tools.cpp
    core/ai/tool_registry.cpp
    core/ai/ai_tool.h
    core/ai/tools/runtime/tool_policy.h
    core/ai/tools/runtime/tool_policy.cpp
    core/ai/tools/runtime/tool_result_sanitizer.h
    core/ai/tools/runtime/tool_result_sanitizer.cpp
    core/ai/tools/runtime/tool_runtime.h
    core/ai/tools/runtime/tool_runtime.cpp
    core/ai/tools/environment_tools.h
    core/ai/tools/environment_tools.cpp
    include/ai_types.h
)

target_include_directories(file_web_tool_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
    ${CMAKE_CURRENT_SOURCE_DIR}/core/ai/tools
)

target_link_libraries(file_web_tool_tests
    Qt::Core
    Qt::Network
    Qt::Test
)

add_test(NAME FileWebToolTests COMMAND file_web_tool_tests)

if (WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    # ---- 判断 Debug 后缀 ----
    set(DEBUG_SUFFIX "")
    if (MSVC AND CMAKE_BUILD_TYPE MATCHES "Debug")
        # MSVC 下 Debug 版 DLL 带 d 后缀
        set(DEBUG_SUFFIX "d")
    endif ()

    # ---- 推断 Qt 安装路径 ----
    set(QT_INSTALL_PATH "${CMAKE_PREFIX_PATH}")
    if (NOT EXISTS "${QT_INSTALL_PATH}/bin")
        set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
        if (NOT EXISTS "${QT_INSTALL_PATH}/bin")
            set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
        endif ()
    endif ()

    message(STATUS "Detected Qt install path: ${QT_INSTALL_PATH}")

    # ---- 平台插件 (qwindows.dll) ----
    if (EXISTS "${QT_INSTALL_PATH}/plugins/platforms/qwindows${DEBUG_SUFFIX}.dll")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/plugins/platforms/")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_INSTALL_PATH}/plugins/platforms/qwindows${DEBUG_SUFFIX}.dll"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/plugins/platforms/")
    elseif (EXISTS "${QT_INSTALL_PATH}/plugins/platforms/qwindows.dll")
        # ✅ MinGW 没有 debug 后缀，补充这一分支
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/plugins/platforms/")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_INSTALL_PATH}/plugins/platforms/qwindows.dll"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/plugins/platforms/")
    endif ()

    # ---- 复制 Qt 核心 DLL ----
    foreach (QT_LIB Core Gui Widgets OpenGL OpenGLWidgets Sql)
        if (EXISTS "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}${DEBUG_SUFFIX}.dll")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}${DEBUG_SUFFIX}.dll"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
        elseif (EXISTS "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}.dll")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}.dll"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
        endif ()
    endforeach ()

    # ---- 调用 windeployqt 自动部署 ----
    if (EXISTS "${QT_INSTALL_PATH}/bin/windeployqt.exe")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND "${QT_INSTALL_PATH}/bin/windeployqt.exe"
                --no-translations
                --no-system-d3d-compiler
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                COMMENT "Running windeployqt to copy Qt runtime dependencies...")
    endif ()

    # ---- 调用 windeployqt 自动部署测试程序 ----
    if (EXISTS "${QT_INSTALL_PATH}/bin/windeployqt.exe")
        add_custom_command(TARGET statistic_tests POST_BUILD
                COMMAND "${QT_INSTALL_PATH}/bin/windeployqt.exe"
                --no-translations
                --no-system-d3d-compiler
                "$<TARGET_FILE:statistic_tests>"
                COMMENT "Running windeployqt for statistic_tests...")
    endif ()

    # ---- 部署 MemoryStrategyTests 的 Qt 运行时依赖 ----
    # memory_strategy_tests 链接 Qt::Sql，单独构建/运行该测试时也需要 Qt6Sql.dll
    # 以及 SQLite 驱动插件；否则 Windows 会以 0xC0000135 报缺失 DLL。
    foreach (QT_LIB Core Sql Test)
        if (EXISTS "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}${DEBUG_SUFFIX}.dll")
            add_custom_command(TARGET memory_strategy_tests POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}${DEBUG_SUFFIX}.dll"
                    "$<TARGET_FILE_DIR:memory_strategy_tests>")
        elseif (EXISTS "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}.dll")
            add_custom_command(TARGET memory_strategy_tests POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}.dll"
                    "$<TARGET_FILE_DIR:memory_strategy_tests>")
        endif ()
    endforeach ()

    if (EXISTS "${QT_INSTALL_PATH}/plugins/sqldrivers/qsqlite${DEBUG_SUFFIX}.dll")
        add_custom_command(TARGET memory_strategy_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:memory_strategy_tests>/sqldrivers/")
        add_custom_command(TARGET memory_strategy_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_INSTALL_PATH}/plugins/sqldrivers/qsqlite${DEBUG_SUFFIX}.dll"
            "$<TARGET_FILE_DIR:memory_strategy_tests>/sqldrivers/")
    elseif (EXISTS "${QT_INSTALL_PATH}/plugins/sqldrivers/qsqlite.dll")
        add_custom_command(TARGET memory_strategy_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:memory_strategy_tests>/sqldrivers/")
        add_custom_command(TARGET memory_strategy_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_INSTALL_PATH}/plugins/sqldrivers/qsqlite.dll"
            "$<TARGET_FILE_DIR:memory_strategy_tests>/sqldrivers/")
    endif ()

    if (EXISTS "${QT_INSTALL_PATH}/bin/windeployqt.exe")
        add_custom_command(TARGET memory_strategy_tests POST_BUILD
                COMMAND "${QT_INSTALL_PATH}/bin/windeployqt.exe"
                --no-translations
                --no-system-d3d-compiler
                "$<TARGET_FILE:memory_strategy_tests>"
                COMMENT "Running windeployqt for memory_strategy_tests...")
    endif ()
endif ()
