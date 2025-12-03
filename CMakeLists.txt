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
        OpenGL
        OpenGLWidgets
        Test
        REQUIRED)

# 收集所有源文件
set(UI_SOURCES
    ui/mainwindow.cpp
    ui/mainwindow.h
    ui/petwindow.cpp
    ui/petwindow.h
    ui/trayicon.cpp
    ui/trayicon.h
)

set(CORE_SOURCES
    core/animation/animation_manager.cpp
    core/animation/animation_manager.h
    core/animation/animation_importer.cpp
    core/animation/animation_importer.h
    core/animation/animation_structs.h

    core/behavior/behavior_manager.cpp
    core/behavior/behavior_manager.h

    core/configLoader/config_manager.cpp
    core/configLoader/config_manager.h

    core/eventHandler/event_handler.cpp
    core/eventHandler/event_handler.h

    core/controller/pet_controller.cpp
    core/controller/pet_controller.h

    core/reminder/pet_reminder.h
    core/reminder/pet_reminder_manager.cpp
    core/reminder/pet_reminder_manager.h
        core/animation/animation_player.cpp
        core/animation/animation_player.h
)

set(ENGINE_SOURCES
    engine/model_loader.cpp
    engine/model_loader.h
    engine/platform_utils.cpp
    engine/platform_utils.h
    engine/render_engine.cpp
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
        statistic/statistic_types.h
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
        Qt::OpenGL
        Qt::OpenGLWidgets
        psapi
)

set(TEST_SOURCES
#        tests/test_statistic_manager.cpp
#        tests/test_pet_reminder_manager.cpp
#        tests/test_animation_importer.cpp
        tests/test_animation_player.cpp
)

add_executable(statistic_tests ${TEST_SOURCES}
                               ${STATISTIC_SOURCES}
                               ${CORE_SOURCES}
                               ${ENTITY_SOURCES}
                               ${ENGINE_SOURCES}
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
        Qt::OpenGL
        Qt::OpenGLWidgets
        psapi
)

enable_testing()
add_test(NAME StatisticTests COMMAND statistic_tests)

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
    foreach (QT_LIB Core Gui Widgets OpenGL OpenGLWidgets)
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
endif ()
