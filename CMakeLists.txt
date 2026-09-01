cmake_minimum_required(VERSION 3.20)
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

# Private reflection is optional. Missing crypto/keychain dependencies must not
# disable the non-private desktop pet targets.
find_path(SODIUM_INCLUDE_DIR sodium.h)
find_library(SODIUM_LIBRARY NAMES sodium libsodium)
find_package(Qt6Keychain QUIET CONFIG)

set(DESKTOP_PET_HAS_LIBSODIUM FALSE)
if(SODIUM_INCLUDE_DIR AND SODIUM_LIBRARY)
    set(DESKTOP_PET_HAS_LIBSODIUM TRUE)
endif()

set(DESKTOP_PET_QTKEYCHAIN_TARGET "")
if(TARGET Qt6Keychain::Keychain)
    set(DESKTOP_PET_QTKEYCHAIN_TARGET Qt6Keychain::Keychain)
elseif(TARGET Qt6Keychain::Qt6Keychain)
    set(DESKTOP_PET_QTKEYCHAIN_TARGET Qt6Keychain::Qt6Keychain)
elseif(TARGET qt6keychain)
    set(DESKTOP_PET_QTKEYCHAIN_TARGET qt6keychain)
endif()

set(DESKTOP_PET_PRIVATE_REFLECTION_AVAILABLE FALSE)
if(DESKTOP_PET_HAS_LIBSODIUM AND DESKTOP_PET_QTKEYCHAIN_TARGET)
    set(DESKTOP_PET_PRIVATE_REFLECTION_AVAILABLE TRUE)
endif()

# 收集所有源文件
set(UI_SOURCES
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
    ui/chat_conversation_model.cpp
    ui/chat_conversation_model.h
    ui/streaming_text_paginator.cpp
    ui/streaming_text_paginator.h
    ui/bubble_playback_controller.cpp
    ui/bubble_playback_controller.h
    ui/thinking_status_selector.cpp
    ui/thinking_status_selector.h
    ui/liquidglasschatbubble.cpp
    ui/liquidglasschatbubble.h
    ui/liquidglasschatbubble_utils.cpp
    ui/liquidglassmaterial.cpp
    ui/liquidglassmaterial.h
    ui/petwindow.cpp
    ui/petwindow_interaction.cpp
    ui/petwindow_emotion.cpp
    ui/petwindow_ai.cpp
    ui/petwindow_screen_chat.cpp
    ui/petwindow_bubble.cpp
    ui/petwindow_native.cpp
    ui/petwindow_snap.cpp
    ui/petwindow.h
    ui/trayicon.cpp
    ui/trayicon.h
)

set(EVENT_LEDGER_SOURCES
    core/ai/domain/domain_result.h
    core/ai/event/event_types.h
    core/ai/event/event_types.cpp
    core/ai/event/event_schema_registry.h
    core/ai/event/event_schema_registry.cpp
    core/ai/event/event_ledger.h
    core/ai/event/event_ledger.cpp
    core/ai/event/event_outbox.h
    core/ai/event/event_outbox.cpp
    core/ai/event/runtime_unit_of_work.h
    core/ai/event/runtime_unit_of_work.cpp
    core/ai/event/sqlite_event_repository.h
    core/ai/event/sqlite_event_repository.cpp
)

set(AGENT_RUNTIME_SOURCES
    core/ai/runtime/runtime_types.h
    core/ai/runtime/runtime_ui_bridge.h
    core/ai/runtime/runtime_ui_bridge.cpp
    core/ai/runtime/agent_runtime_services.h
    core/ai/runtime/agent_runtime_services.cpp
    core/ai/runtime/agent_bootstrap.h
    core/ai/runtime/agent_bootstrap.cpp
)

set(MODEL_ROUTING_SOURCES
    core/ai/model/model_role_registry.h
    core/ai/model/model_role_registry.cpp
    core/ai/model/model_router.h
    core/ai/model/model_router.cpp
    core/ai/context/context_assembler.h
    core/ai/context/context_assembler.cpp
    core/ai/llm/llm_chat_model_client.h
    core/ai/llm/llm_chat_model_client.cpp
)

set(IDENTITY_STORAGE_SOURCES
    core/ai/identity/identity_types.h
    core/ai/identity/identity_types.cpp
    core/ai/identity/sqlite_identity_repository.h
    core/ai/identity/sqlite_identity_repository.cpp
)

set(EMOTION_PROVIDER_SOURCES
    core/ai/integration/emotion_state_provider.h
    core/ai/integration/emotion_state_provider.cpp
)

set(IDENTITY_STATE_SOURCES
    ${IDENTITY_STORAGE_SOURCES}
    ${EMOTION_PROVIDER_SOURCES}
    core/ai/identity/personality_service.h
    core/ai/identity/personality_service.cpp
    core/ai/identity/relationship_service.h
    core/ai/identity/relationship_service.cpp
    core/ai/identity/self_model_service.h
    core/ai/identity/self_model_service.cpp
)

set(REFLECTION_SOURCES
    core/ai/reflection/reflection_types.h
    core/ai/reflection/reflection_types.cpp
    core/ai/reflection/cancellation_token.h
    core/ai/reflection/cancellation_token.cpp
    core/ai/reflection/private_key_provider.h
    core/ai/reflection/private_key_provider.cpp
    core/ai/reflection/private_psyche_crypto.h
    core/ai/reflection/private_psyche_crypto.cpp
    core/ai/reflection/sqlite_private_psyche_repository.h
    core/ai/reflection/sqlite_private_psyche_repository.cpp
    core/ai/reflection/inner_thought_service.h
    core/ai/reflection/inner_thought_service.cpp
    core/ai/reflection/daydream_sleep_adapter.h
    core/ai/reflection/daydream_sleep_adapter.cpp
    core/ai/reflection/diary_service.h
    core/ai/reflection/diary_service.cpp
    core/ai/reflection/sleep_session_repository.h
    core/ai/reflection/sleep_session_repository.cpp
    core/ai/reflection/sleep_cycle_coordinator.h
    core/ai/reflection/sleep_cycle_coordinator.cpp
)

set(OWNER_DIARY_SOURCES
    core/ai/owner/owner_diary_protocol.h
    core/ai/owner/owner_diary_protocol.cpp
    core/ai/owner/owner_diary_facade.h
    core/ai/owner/owner_diary_facade.cpp
    core/ai/owner/owner_diary_server.h
    core/ai/owner/owner_diary_server.cpp
)

set(CORE_SOURCES
    core/animation/animation_manager.cpp
    core/animation/animation_manager.h
    core/animation/animation_importer.cpp
    core/animation/animation_importer.h

    core/behavior/behavior_manager.cpp
    core/behavior/behavior_manager.h

    core/emotion/emotion_types.cpp
    core/emotion/emotion_types.h
    core/emotion/emotion_config_json.cpp
    core/emotion/emotion_config_json.h
    core/emotion/emotion_state_repository.h
    core/emotion/sqlite_emotion_state_repository.cpp
    core/emotion/sqlite_emotion_state_repository.h
    core/emotion/emotion_engine.cpp
    core/emotion/emotion_engine.h

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
    core/ai/llm/llm_stream_types.h
    core/ai/llm/sse_event_parser.h
    core/ai/llm/sse_event_parser.cpp
    core/ai/llm/anthropic_messages_client.h
    core/ai/llm/anthropic_messages_client.cpp
    core/ai/llm/openai_compatible_client.h
    core/ai/llm/openai_compatible_client.cpp
    core/ai/llm/llm_chat_service.h
    core/ai/llm/llm_chat_service.cpp

    core/ai/chat/chat_types.h
    core/ai/chat/chat_types.cpp
    core/ai/chat/profile_chat_history_store.h
    core/ai/chat/profile_chat_history_store.cpp
    core/ai/chat/launcher_chat_server.h
    core/ai/chat/launcher_chat_server.cpp

    core/ai/context_builder.h
    core/ai/context_builder.cpp
    core/ai/identity/identity_baseline.h
    core/ai/identity/identity_baseline.cpp
    core/ai/identity/persona_projector.h
    core/ai/identity/persona_projector.cpp
    core/ai/ai_call_logger.h
    core/ai/ai_call_logger.cpp
    core/ai/ai_brain.h
    core/ai/ai_brain.cpp
    core/ai/ai_brain_router.cpp
    core/ai/ai_brain_loop.cpp

    core/ai/runtime/profile_resolver.h
    core/ai/runtime/profile_resolver.cpp
    core/ai/runtime/profile_data_migrator.h
    core/ai/runtime/profile_data_migrator.cpp
    ${EVENT_LEDGER_SOURCES}
    ${AGENT_RUNTIME_SOURCES}
    ${MODEL_ROUTING_SOURCES}
    ${IDENTITY_STATE_SOURCES}
    ${REFLECTION_SOURCES}
    ${OWNER_DIARY_SOURCES}

    core/ai/ai_tool.h
    core/ai/tool_registry.cpp
    core/ai/tool_registry.h
    core/ai/agent/agent_state.h
    core/ai/agent/agent_session.h
    core/ai/agent/agent_session.cpp
    core/ai/router/intent_types.h
    core/ai/router/intent_router.h
    core/ai/router/intent_router.cpp
    core/ai/scheduler/scheduled_task.h
    core/ai/scheduler/scheduled_task.cpp
    core/ai/scheduler/agent_scheduler.h
    core/ai/scheduler/agent_scheduler.cpp
    core/ai/scheduler/daydream_trigger_policy.h
    core/ai/scheduler/daydream_trigger_policy.cpp
    core/ai/tools/runtime/tool_policy.h
    core/ai/tools/runtime/tool_policy.cpp
    core/ai/tools/runtime/tool_result_sanitizer.h
    core/ai/tools/runtime/tool_result_sanitizer.cpp
    core/ai/tools/runtime/tool_runtime.h
    core/ai/tools/runtime/tool_runtime.cpp
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
    core/ai/memory/tag_cooccurrence_graph.h
    core/ai/memory/tag_cooccurrence_graph.cpp
    core/ai/memory/working_memory_cache.h
    core/ai/memory/working_memory_cache.cpp
    core/ai/memory/daydream_consolidator.h
    core/ai/memory/daydream_consolidator.cpp
    core/ai/memory/embedding_provider.h
    core/ai/memory/embedding_index.h
    core/ai/memory/noop_embedding_index.h
    core/ai/memory/partition_policy.h
    core/ai/memory/sqlite_embedding_index.h
    core/ai/memory/sqlite_embedding_index.cpp
    core/ai/memory/model_downloader.h
    core/ai/memory/model_downloader.cpp
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

    core/ai/prompt/prompt_template_types.h
    core/ai/prompt/prompt_template_types.cpp
    core/ai/prompt/prompt_template_store.h
    core/ai/prompt/prompt_template_store.cpp
    core/ai/prompt/prompt_renderer.h
    core/ai/prompt/prompt_renderer.cpp
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
        $<$<PLATFORM_ID:Windows>:psapi>
)

function(configure_private_reflection target_name)
    target_link_libraries(${target_name} $<$<PLATFORM_ID:Windows>:advapi32>)
    if(DESKTOP_PET_HAS_LIBSODIUM)
        target_include_directories(${target_name} PRIVATE ${SODIUM_INCLUDE_DIR})
        target_link_libraries(${target_name} ${SODIUM_LIBRARY})
        target_compile_definitions(${target_name} PRIVATE DESKTOP_PET_HAS_LIBSODIUM=1)
    else()
        target_compile_definitions(${target_name} PRIVATE DESKTOP_PET_HAS_LIBSODIUM=0)
    endif()

    if(DESKTOP_PET_QTKEYCHAIN_TARGET)
        target_link_libraries(${target_name} ${DESKTOP_PET_QTKEYCHAIN_TARGET})
        target_compile_definitions(${target_name} PRIVATE DESKTOP_PET_HAS_QTKEYCHAIN=1)
    else()
        target_compile_definitions(${target_name} PRIVATE DESKTOP_PET_HAS_QTKEYCHAIN=0)
    endif()

    if(DESKTOP_PET_PRIVATE_REFLECTION_AVAILABLE)
        target_compile_definitions(${target_name} PRIVATE DESKTOP_PET_HAS_PRIVATE_REFLECTION=1)
    else()
        target_compile_definitions(${target_name} PRIVATE DESKTOP_PET_HAS_PRIVATE_REFLECTION=0)
    endif()
endfunction()

configure_private_reflection(Desktop_Pet)
if(DESKTOP_PET_PRIVATE_REFLECTION_AVAILABLE)
    message(STATUS "Private reflection capability available")
else()
    message(STATUS "Private reflection capability unavailable; non-private features remain enabled")
endif()

enable_testing()

function(add_desktop_pet_cpp_test target_name ctest_name)
    set(options APPLE_CORE_GRAPHICS PRIVATE_REFLECTION)
    set(multi_value_args SOURCES INCLUDE_DIRS LIBRARIES)
    cmake_parse_arguments(CPP_TEST "${options}" "" "${multi_value_args}" ${ARGN})

    add_executable(${target_name} ${CPP_TEST_SOURCES})
    target_include_directories(${target_name} PRIVATE ${CPP_TEST_INCLUDE_DIRS})
    target_link_libraries(${target_name} ${CPP_TEST_LIBRARIES})

    if(CPP_TEST_APPLE_CORE_GRAPHICS AND APPLE)
        target_link_libraries(${target_name} "-framework CoreGraphics")
    endif()
    if(CPP_TEST_PRIVATE_REFLECTION)
        configure_private_reflection(${target_name})
    endif()

    add_test(NAME ${ctest_name} COMMAND ${target_name})
endfunction()

set(TEST_SOURCES
#        tests/test_statistic_manager.cpp
#        tests/test_pet_reminder_manager.cpp
#        tests/test_animation_importer.cpp
        tests/test_animation_player.cpp
)

add_desktop_pet_cpp_test(statistic_tests StatisticTests
    SOURCES
        ${TEST_SOURCES}
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
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/entity
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinygltf
    LIBRARIES
        Qt::Core
        Qt::Test
        Qt::Widgets
        Qt::Network
        Qt::OpenGL
        Qt::OpenGLWidgets
        Qt::Sql
        $<$<PLATFORM_ID:Windows>:psapi>
)

if(WIN32
    AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.venv/Scripts/python.exe"
    AND (NOT Python3_EXECUTABLE OR NOT EXISTS "${Python3_EXECUTABLE}"))
    set(Python3_EXECUTABLE
        "${CMAKE_CURRENT_SOURCE_DIR}/.venv/Scripts/python.exe"
        CACHE FILEPATH "Python interpreter used by Desktop Pet tests" FORCE)
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME LauncherConfigTests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_launcher_config.py)
    set_tests_properties(LauncherConfigTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME PetProfileIdTests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pet_profile_id.py)
    set_tests_properties(PetProfileIdTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME LauncherProcessTrackerTests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_launcher_process_tracker.py)
    set_tests_properties(LauncherProcessTrackerTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ModelRoleConfigTests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_model_role_config.py)
    set_tests_properties(ModelRoleConfigTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ApiConnectionTesterTests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_api_connection_tester.py)
    set_tests_properties(ApiConnectionTesterTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME LauncherChatTests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_launcher_chat.py)
    set_tests_properties(LauncherChatTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ---- Profile identity and legacy memory migration tests ----
add_desktop_pet_cpp_test(profile_data_migrator_tests ProfileDataMigratorTests
    SOURCES
        tests/test_profile_data_migrator.cpp
        core/ai/runtime/profile_resolver.h
        core/ai/runtime/profile_resolver.cpp
        core/ai/runtime/profile_data_migrator.h
        core/ai/runtime/profile_data_migrator.cpp
        entity/pet.h
        entity/pet.cpp
        core/ai/memory/memory_types.h
        core/ai/memory/memory_store.h
        core/ai/memory/memory_store.cpp
        core/ai/memory/memory_repository.h
        core/ai/memory/sqlite_memory_repository.h
        core/ai/memory/sqlite_memory_repository.cpp
        core/ai/memory/memory_relation.h
        core/ai/memory/memory_relation_graph.h
        core/ai/memory/memory_relation_graph.cpp
        core/ai/memory/tag_cooccurrence_graph.h
        core/ai/memory/tag_cooccurrence_graph.cpp
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/entity
    LIBRARIES
        Qt::Core
        Qt::Sql
        Qt::Test
)

# ---- Event ledger, checkpoint, outbox, and unit-of-work tests ----
add_desktop_pet_cpp_test(event_ledger_tests EventLedgerTests
    SOURCES
        tests/test_event_ledger.cpp
        ${EVENT_LEDGER_SOURCES}
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
    LIBRARIES
        Qt::Core
        Qt::Sql
        Qt::Test
)

# ---- Agent bootstrap and runtime service composition test support ----
set(AGENT_RUNTIME_TEST_SUPPORT_SOURCES
    ${EVENT_LEDGER_SOURCES}
    ${AGENT_RUNTIME_SOURCES}
    ${MODEL_ROUTING_SOURCES}
    ${IDENTITY_STATE_SOURCES}
    ${REFLECTION_SOURCES}
    ${OWNER_DIARY_SOURCES}
    core/ai/runtime/profile_resolver.h
    core/ai/runtime/profile_resolver.cpp
    core/ai/runtime/profile_data_migrator.h
    core/ai/runtime/profile_data_migrator.cpp
    core/ai/ai_brain.h
    core/ai/ai_brain.cpp
    core/ai/ai_brain_router.cpp
    core/ai/ai_brain_loop.cpp
    core/ai/ai_call_logger.h
    core/ai/ai_call_logger.cpp
    core/ai/agent/agent_session.h
    core/ai/agent/agent_session.cpp
    core/ai/context_builder.h
    core/ai/context_builder.cpp
    core/ai/identity/identity_baseline.h
    core/ai/identity/identity_baseline.cpp
    core/ai/identity/persona_projector.h
    core/ai/identity/persona_projector.cpp
    core/ai/llm/llm_client.h
    core/ai/llm/llm_stream_types.h
    core/ai/llm/sse_event_parser.h
    core/ai/llm/sse_event_parser.cpp
    core/ai/llm/anthropic_messages_client.h
    core/ai/llm/anthropic_messages_client.cpp
    core/ai/llm/openai_compatible_client.h
    core/ai/llm/openai_compatible_client.cpp
    core/ai/llm/llm_chat_service.h
    core/ai/llm/llm_chat_service.cpp
    core/ai/prompt/prompt_template_types.h
    core/ai/prompt/prompt_template_types.cpp
    core/ai/prompt/prompt_renderer.h
    core/ai/prompt/prompt_renderer.cpp
    core/ai/router/intent_types.h
    core/ai/router/intent_router.h
    core/ai/router/intent_router.cpp
    core/ai/tool_registry.h
    core/ai/tool_registry.cpp
    core/ai/tools/runtime/tool_policy.h
    core/ai/tools/runtime/tool_policy.cpp
    core/ai/tools/runtime/tool_result_sanitizer.h
    core/ai/tools/runtime/tool_result_sanitizer.cpp
    core/ai/tools/runtime/tool_runtime.h
    core/ai/tools/runtime/tool_runtime.cpp
    core/ai/tools/environment_tools.h
    core/ai/tools/environment_tools.cpp
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
    core/ai/memory/memory_relation.h
    core/ai/memory/memory_relation_graph.h
    core/ai/memory/memory_relation_graph.cpp
    core/ai/memory/tag_cooccurrence_graph.h
    core/ai/memory/tag_cooccurrence_graph.cpp
    core/ai/memory/working_memory_cache.h
    core/ai/memory/working_memory_cache.cpp
    core/ai/memory/daydream_consolidator.h
    core/ai/memory/daydream_consolidator.cpp
    core/ai/memory/embedding_provider.h
    core/ai/memory/embedding_index.h
    core/ai/memory/partition_policy.h
    core/ai/scheduler/scheduled_task.h
    core/ai/scheduler/scheduled_task.cpp
    core/ai/scheduler/agent_scheduler.h
    core/ai/scheduler/agent_scheduler.cpp
    core/ai/scheduler/daydream_trigger_policy.h
    core/ai/scheduler/daydream_trigger_policy.cpp
    core/ai/skill/skill_types.h
    core/ai/skill/skill_types.cpp
    core/ai/skill/skill_store.h
    core/ai/skill/skill_store.cpp
    core/ai/skill/skill_matcher.h
    core/ai/skill/skill_matcher.cpp
    core/configLoader/config_manager.h
    core/configLoader/config_manager.cpp
    core/emotion/emotion_types.h
    core/emotion/emotion_types.cpp
    core/emotion/emotion_config_json.h
    core/emotion/emotion_config_json.cpp
    core/emotion/emotion_engine.h
    core/emotion/emotion_engine.cpp
    statistic/statistic_manager.h
    statistic/statistic_manager.cpp
    include/statistic_types.h
    include/ai_types.h
    include/global_types.h
)

add_desktop_pet_cpp_test(agent_runtime_services_tests AgentRuntimeServicesTests
    SOURCES
        tests/test_agent_runtime_services.cpp
        ${AGENT_RUNTIME_TEST_SUPPORT_SOURCES}
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
    LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Sql
        Qt::Test
        $<$<PLATFORM_ID:Windows>:psapi>
    APPLE_CORE_GRAPHICS
    PRIVATE_REFLECTION
)

# ---- Emotion provider read-only contract tests ----
add_desktop_pet_cpp_test(emotion_provider_contract_tests EmotionProviderContractTests
    SOURCES
        tests/test_emotion_provider_contract.cpp
        ${EMOTION_PROVIDER_SOURCES}
        core/emotion/emotion_types.h
        core/emotion/emotion_types.cpp
        core/emotion/emotion_state_repository.h
        core/emotion/emotion_engine.h
        core/emotion/emotion_engine.cpp
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
    LIBRARIES
        Qt::Core
        Qt::Test
)

# ---- Identity state, projection, and runtime composition tests ----
add_desktop_pet_cpp_test(identity_state_tests IdentityStateTests
    SOURCES
        tests/test_identity_state.cpp
        ${AGENT_RUNTIME_TEST_SUPPORT_SOURCES}
        entity/pet_personality.h
        entity/pet_personality.cpp
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/entity
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
    LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Sql
        Qt::Test
        $<$<PLATFORM_ID:Windows>:psapi>
    APPLE_CORE_GRAPHICS
    PRIVATE_REFLECTION
)

# ---- Reflection, private diary, and sleep coordinator tests ----
add_desktop_pet_cpp_test(sleep_cycle_tests SleepCycleTests
    SOURCES
        tests/test_sleep_cycle.cpp
        ${AGENT_RUNTIME_TEST_SUPPORT_SOURCES}
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
    LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Sql
        Qt::Test
        $<$<PLATFORM_ID:Windows>:psapi>
    APPLE_CORE_GRAPHICS
    PRIVATE_REFLECTION
)

# ---- Read-only owner diary facade, IPC authentication, and model invisibility ----
add_desktop_pet_cpp_test(owner_diary_server_tests OwnerDiaryServerTests
    SOURCES
        tests/test_owner_diary_server.cpp
        ${AGENT_RUNTIME_TEST_SUPPORT_SOURCES}
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
    LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Sql
        Qt::Test
        $<$<PLATFORM_ID:Windows>:psapi>
    APPLE_CORE_GRAPHICS
    PRIVATE_REFLECTION
)

if(Python3_Interpreter_FOUND)
    add_test(NAME OwnerDiaryClientTests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_owner_diary_client.py)
    set_tests_properties(OwnerDiaryClientTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ---- Model role routing, output repair, and context projection tests ----
add_desktop_pet_cpp_test(model_router_tests ModelRouterTests
    SOURCES
        tests/test_model_router.cpp
        ${MODEL_ROUTING_SOURCES}
        core/ai/llm/llm_client.h
        core/ai/llm/llm_stream_types.h
        core/ai/llm/sse_event_parser.h
        core/ai/llm/sse_event_parser.cpp
        core/ai/llm/anthropic_messages_client.h
        core/ai/llm/anthropic_messages_client.cpp
        core/ai/llm/openai_compatible_client.h
        core/ai/llm/openai_compatible_client.cpp
        core/ai/llm/llm_chat_service.h
        core/ai/llm/llm_chat_service.cpp
        core/configLoader/config_manager.h
        core/configLoader/config_manager.cpp
        core/ai/identity/identity_baseline.h
        core/ai/identity/identity_baseline.cpp
        core/ai/identity/identity_types.h
        core/ai/identity/identity_types.cpp
        core/emotion/emotion_types.h
        core/emotion/emotion_types.cpp
        core/emotion/emotion_config_json.h
        core/emotion/emotion_config_json.cpp
        statistic/statistic_manager.h
        statistic/statistic_manager.cpp
        include/statistic_types.h
        include/ai_types.h
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
    LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Test
)

# ---- Dialogue streaming orchestration tests (no ONNX dependency) ----
add_desktop_pet_cpp_test(streaming_dialogue_tests StreamingDialogueTests
    SOURCES
        tests/test_streaming_dialogue.cpp
        ${AGENT_RUNTIME_TEST_SUPPORT_SOURCES}
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
    LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Sql
        Qt::Test
        $<$<PLATFORM_ID:Windows>:psapi>
    APPLE_CORE_GRAPHICS
    PRIVATE_REFLECTION
)

# ---- Profile-scoped chat persistence tests (no ONNX dependency) ----
add_desktop_pet_cpp_test(chat_history_tests ChatHistoryTests
    SOURCES
        tests/test_chat_history.cpp
        core/ai/chat/chat_types.h
        core/ai/chat/chat_types.cpp
        core/ai/chat/profile_chat_history_store.h
        core/ai/chat/profile_chat_history_store.cpp
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/core
    LIBRARIES
        Qt::Core
        Qt::Test
)

# ---- Shared streaming conversation model tests (no ONNX dependency) ----
add_desktop_pet_cpp_test(chat_ui_model_tests ChatUiModelTests
    SOURCES
        tests/test_chat_conversation_model.cpp
        ui/chat_conversation_model.h
        ui/chat_conversation_model.cpp
        core/ai/chat/chat_types.h
        core/ai/chat/chat_types.cpp
        core/ai/chat/profile_chat_history_store.h
        core/ai/chat/profile_chat_history_store.cpp
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/core
    LIBRARIES
        Qt::Core
        Qt::Test
)

# ---- Fluent chat history window tests (no ONNX dependency) ----
add_desktop_pet_cpp_test(chat_history_window_tests ChatHistoryWindowTests
    SOURCES
        tests/test_chat_history_window.cpp
        ui/chat_history_window.h
        ui/chat_history_window.cpp
        ui/chat_conversation_model.h
        ui/chat_conversation_model.cpp
        ui/theme_manager.h
        ui/theme_manager.cpp
        core/ai/chat/chat_types.h
        core/ai/chat/chat_types.cpp
        core/ai/chat/profile_chat_history_store.h
        core/ai/chat/profile_chat_history_store.cpp
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/core
    LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
)

# ---- Streaming desktop bubble pagination tests (no ONNX dependency) ----
add_desktop_pet_cpp_test(streaming_text_paginator_tests StreamingTextPaginatorTests
    SOURCES
        tests/test_streaming_text_paginator.cpp
        ui/streaming_text_paginator.h
        ui/streaming_text_paginator.cpp
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
    LIBRARIES
        Qt::Core
        Qt::Test
)

# ---- Streaming desktop bubble playback and widget tests (no ONNX dependency) ----
add_desktop_pet_cpp_test(bubble_playback_controller_tests BubblePlaybackControllerTests
    SOURCES
        tests/test_bubble_playback_controller.cpp
        ui/bubble_playback_controller.h
        ui/bubble_playback_controller.cpp
        ui/thinking_status_selector.h
        ui/thinking_status_selector.cpp
        ui/liquidglasschatbubble.h
        ui/liquidglasschatbubble.cpp
        ui/liquidglasschatbubble_utils.cpp
        ui/liquidglassmaterial.h
        ui/liquidglassmaterial.cpp
        include/ai_types.h
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
    LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    APPLE_CORE_GRAPHICS
)

add_desktop_pet_cpp_test(statistic_manager_tests StatisticManagerTests
    SOURCES
        tests/test_statistic_manager.cpp
        statistic/statistic_manager.cpp
        statistic/statistic_manager.h
        include/statistic_types.h
        include/ai_types.h
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
    LIBRARIES
        Qt::Core
        Qt::Test
        Qt::Widgets
)

add_desktop_pet_cpp_test(model_loader_tests ModelLoaderTests
    SOURCES
        tests/test_model_loader.cpp
        engine/model_loader.cpp
        engine/model_loader_geometry.cpp
        engine/model_loader_skeleton.cpp
        engine/model_loader.h
        ${TINYGLTF_SOURCES}
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinygltf
    LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Test
)

# ---- Tool 系统测试 ----
# 轻量测试，不依赖 OpenGL / 模型文件
add_desktop_pet_cpp_test(tool_tests ToolTests
    SOURCES
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
        core/ai/scheduler/scheduled_task.h
        core/ai/scheduler/scheduled_task.cpp
        core/ai/scheduler/agent_scheduler.h
        core/ai/scheduler/agent_scheduler.cpp
        include/ai_types.h
        ${STATISTIC_SOURCES}
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai/tools
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
        ${CMAKE_CURRENT_SOURCE_DIR}/entity
    LIBRARIES
        Qt::Core
        Qt::Network
        Qt::Test
)

# ---- Intent 路由测试 ----
add_desktop_pet_cpp_test(intent_router_tests IntentRouterTests
    SOURCES
        tests/test_intent_router.cpp
        core/ai/router/intent_router.h
        core/ai/router/intent_router.cpp
        core/ai/router/intent_types.h
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
    LIBRARIES
        Qt::Core
        Qt::Test
)

# ---- Emotion deterministic core tests ----
add_desktop_pet_cpp_test(emotion_tests EmotionTests
    SOURCES
        tests/test_emotion_engine.cpp
        core/emotion/emotion_types.cpp
        core/emotion/emotion_types.h
        core/emotion/emotion_config_json.cpp
        core/emotion/emotion_config_json.h
        core/emotion/emotion_state_repository.h
        core/emotion/emotion_engine.cpp
        core/emotion/emotion_engine.h
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/core
    LIBRARIES
        Qt::Core
        Qt::Test
)

# ---- Emotion runtime adapters and SQLite tests ----
add_desktop_pet_cpp_test(emotion_runtime_tests EmotionRuntimeTests
    SOURCES
        tests/test_emotion_runtime.cpp
        core/emotion/emotion_types.cpp
        core/emotion/emotion_types.h
        core/emotion/emotion_engine.cpp
        core/emotion/emotion_engine.h
        core/emotion/emotion_state_repository.h
        core/emotion/sqlite_emotion_state_repository.cpp
        core/emotion/sqlite_emotion_state_repository.h
        core/behavior/behavior_manager.cpp
        core/behavior/behavior_manager.h
        core/controller/pet_controller.cpp
        core/controller/pet_controller.h
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/core
    LIBRARIES
        Qt::Core
        Qt::Sql
        Qt::Test
)

# ---- Memory 写入策略测试 ----
add_desktop_pet_cpp_test(memory_strategy_tests MemoryStrategyTests
    SOURCES
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
        core/ai/memory/tag_cooccurrence_graph.h
        core/ai/memory/tag_cooccurrence_graph.cpp
        core/ai/memory/working_memory_cache.h
        core/ai/memory/working_memory_cache.cpp
        core/ai/tools/memory_tools.h
        core/ai/tools/memory_tools.cpp
        core/ai/memory/embedding_provider.h
        core/ai/memory/embedding_index.h
        core/ai/memory/noop_embedding_index.h
        core/ai/memory/partition_policy.h
        core/ai/memory/sqlite_embedding_index.h
        core/ai/memory/sqlite_embedding_index.cpp
        core/ai/memory/model_downloader.h
        core/ai/memory/model_downloader.cpp
        core/ai/memory/daydream_consolidator.h
        core/ai/memory/daydream_consolidator.cpp
        core/ai/scheduler/daydream_trigger_policy.h
        core/ai/scheduler/daydream_trigger_policy.cpp
        core/ai/skill/skill_types.h
        core/ai/skill/skill_types.cpp
        core/ai/skill/skill_store.h
        core/ai/skill/skill_store.cpp
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
    LIBRARIES
        Qt::Core
        Qt::Sql
        Qt::Network
        Qt::Test
    APPLE_CORE_GRAPHICS
)

# ---- onnxruntime (third_party, optional软依赖) ----
# 探测 third_party/onnxruntime/onnxruntime-<platform>-<ver>/，找到则把
# OnnxEmbeddingProvider 编入 memory_strategy_tests；找不到则跳过（provider
# 不存在，语义检索退化为 Noop），不阻塞构建。
set(ORT_ROOT "")
set(ORT_LIB "")
file(GLOB _ort_dirs "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/onnxruntime-*")
foreach(d ${_ort_dirs})
    if(EXISTS "${d}/include/onnxruntime_cxx_api.h")
        set(ORT_ROOT "${d}")
        break()
    endif()
endforeach()
if(ORT_ROOT)
    find_library(ORT_LIB onnxruntime NO_DEFAULT_PATH PATHS "${ORT_ROOT}/lib")
    if(NOT ORT_LIB)
        # macOS 包名带版本，再放宽找一次
        file(GLOB _ort_lib "${ORT_ROOT}/lib/libonnxruntime*.dylib" "${ORT_ROOT}/lib/libonnxruntime*.so" "${ORT_ROOT}/lib/onnxruntime*.dll")
        if(_ort_lib)
            list(GET _ort_lib 0 ORT_LIB)
            set(ORT_LIB ${ORT_LIB} CACHE FILEPATH "")
        endif()
    endif()
endif()
if(ORT_ROOT AND ORT_LIB)
    set(ORT_FOUND TRUE CACHE BOOL "onnxruntime available")
    message(STATUS "onnxruntime found: root=${ORT_ROOT} lib=${ORT_LIB}")
    target_sources(memory_strategy_tests PRIVATE
        core/ai/memory/onnx_embedding_provider.h
        core/ai/memory/onnx_embedding_provider.cpp)
    target_include_directories(memory_strategy_tests PRIVATE ${ORT_ROOT}/include)
    target_link_libraries(memory_strategy_tests ${ORT_LIB})
    target_compile_definitions(memory_strategy_tests PRIVATE
        DESKTOP_PET_HAS_ORT
        DESKTOP_PET_EMBEDDING_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets/embeddings")
    # 运行时让 binary 在同目录 / 相对 lib 找到动态库（mac 需要）
    set_target_properties(memory_strategy_tests PROPERTIES
        BUILD_RPATH "${ORT_ROOT}/lib"
        INSTALL_RPATH "@loader_path")
else()
    set(ORT_FOUND FALSE CACHE BOOL "onnxruntime available")
    message(STATUS "onnxruntime NOT found — OnnxEmbeddingProvider skipped (Noop fallback). "
                   "Run tools/fetch_onnxruntime.sh to enable.")
    # 不加 onnx_embedding_provider.cpp，但保留头以供 Desktop_Pet 在 ort 可用时引用
endif()

# ---- LLM 异步服务测试 ----
add_desktop_pet_cpp_test(llm_tests LlmTests
    SOURCES
        tests/test_llm_chat_service.cpp
        core/ai/llm/llm_chat_service.h
        core/ai/llm/llm_chat_service.cpp
        core/ai/llm/llm_client.h
        core/ai/llm/llm_stream_types.h
        core/ai/llm/sse_event_parser.h
        core/ai/llm/sse_event_parser.cpp
        core/ai/llm/anthropic_messages_client.h
        core/ai/llm/anthropic_messages_client.cpp
        core/ai/llm/openai_compatible_client.h
        core/ai/llm/openai_compatible_client.cpp
        core/ai/context_builder.h
        core/ai/context_builder.cpp
        core/ai/identity/identity_baseline.h
        core/ai/identity/identity_baseline.cpp
        core/ai/identity/persona_projector.h
        core/ai/identity/persona_projector.cpp
        ${IDENTITY_STORAGE_SOURCES}
        core/ai/prompt/prompt_template_types.h
        core/ai/prompt/prompt_renderer.h
        core/ai/prompt/prompt_renderer.cpp
        core/configLoader/config_manager.h
        core/configLoader/config_manager.cpp
        core/emotion/emotion_types.cpp
        core/emotion/emotion_types.h
        core/emotion/emotion_config_json.cpp
        core/emotion/emotion_config_json.h
        statistic/statistic_manager.h
        statistic/statistic_manager.cpp
        include/statistic_types.h
        include/ai_types.h
        include/global_types.h
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai/llm
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
    LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Sql
        Qt::Test
)

# ---- Anthropic Messages SSE protocol tests (no ONNX dependency) ----
add_desktop_pet_cpp_test(anthropic_messages_client_tests AnthropicMessagesClientTests
    SOURCES
        tests/test_anthropic_messages_client.cpp
        core/ai/llm/llm_stream_types.h
        core/ai/llm/sse_event_parser.h
        core/ai/llm/sse_event_parser.cpp
        core/ai/llm/anthropic_messages_client.h
        core/ai/llm/anthropic_messages_client.cpp
        core/ai/llm/llm_client.h
        core/ai/llm/openai_compatible_client.h
        core/ai/llm/openai_compatible_client.cpp
        include/ai_types.h
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai/llm
    LIBRARIES
        Qt::Core
        Qt::Network
        Qt::Test
)

# ---- 提示词模版 + 身份基线测试 ----
# 验证 IdentityBaseline、PersonaProjector、PromptRenderer 与固定安全边界。
add_desktop_pet_cpp_test(prompt_tests PromptRendererTests
    SOURCES
        tests/test_prompt_renderer.cpp
        core/ai/prompt/prompt_template_types.h
        core/ai/prompt/prompt_template_types.cpp
        core/ai/prompt/prompt_template_store.h
        core/ai/prompt/prompt_template_store.cpp
        core/ai/prompt/prompt_renderer.h
        core/ai/prompt/prompt_renderer.cpp
        core/ai/identity/identity_baseline.h
        core/ai/identity/identity_baseline.cpp
        core/ai/identity/persona_projector.h
        core/ai/identity/persona_projector.cpp
        ${IDENTITY_STORAGE_SOURCES}
        core/ai/context_builder.h
        core/ai/context_builder.cpp
        core/configLoader/config_manager.h
        core/configLoader/config_manager.cpp
        core/emotion/emotion_types.cpp
        core/emotion/emotion_types.h
        core/emotion/emotion_config_json.cpp
        core/emotion/emotion_config_json.h
        statistic/statistic_manager.h
        statistic/statistic_manager.cpp
        include/statistic_types.h
        include/ai_types.h
        include/global_types.h
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/statistic
    LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Sql
        Qt::Test
)

# ---- 文件和网络工具测试 ----
add_desktop_pet_cpp_test(file_web_tool_tests FileWebToolTests
    SOURCES
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
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ai/tools
    LIBRARIES
        Qt::Core
        Qt::Network
        Qt::Test
    APPLE_CORE_GRAPHICS
)

if (WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    # ---- 判断 Debug 后缀 ----
    set(DEBUG_SUFFIX "")
    if (MSVC AND CMAKE_BUILD_TYPE MATCHES "Debug")
        # MSVC 下 Debug 版 DLL 带 d 后缀
        set(DEBUG_SUFFIX "d")
    endif ()

    # ---- 推断 Qt 安装路径 ----
    set(QT_INSTALL_PATH "")
    if (TARGET Qt6::qmake)
        get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
        if (QT_QMAKE_EXECUTABLE)
            get_filename_component(QT_BIN_PATH "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
            get_filename_component(QT_INSTALL_PATH "${QT_BIN_PATH}" DIRECTORY)
        endif ()
    endif ()
    if (NOT EXISTS "${QT_INSTALL_PATH}/bin" AND Qt6Core_DIR)
        get_filename_component(
            QT_INSTALL_PATH "${Qt6Core_DIR}/../../.." ABSOLUTE)
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
    foreach (QT_LIB Core Gui Widgets Network OpenGL OpenGLWidgets Sql)
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
                "$<TARGET_FILE:${PROJECT_NAME}>"
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

    if (EXISTS "${QT_INSTALL_PATH}/bin/windeployqt.exe")
        add_custom_command(TARGET emotion_runtime_tests POST_BUILD
                COMMAND "${QT_INSTALL_PATH}/bin/windeployqt.exe"
                --no-translations
                --no-system-d3d-compiler
                "$<TARGET_FILE:emotion_runtime_tests>"
                COMMENT "Running windeployqt for emotion_runtime_tests...")
    endif ()
endif ()
