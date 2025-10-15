cmake_minimum_required(VERSION 4.0)
project(Desktop_Pet)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_PREFIX_PATH "E:/Qt/6.5.3/mingw_64/lib/cmake")

find_package(Qt6 COMPONENTS
        Core
        Gui
        Widgets
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
    core/animation_manager.cpp
    core/animation_manager.h
    core/behavior_manager.cpp
    core/behavior_manager.h
    core/config_manager.cpp
    core/config_manager.h
    core/event_handler.cpp
    core/event_handler.h
    core/pet_controller.cpp
    core/pet_controller.h
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
)

set(INCLUDE_SOURCES
    include/common.h
    include/global_types.h
    include/macros.h
)

# 合并所有源文件
set(ALL_SOURCES
    main.cpp
    ${UI_SOURCES}
    ${CORE_SOURCES}
    ${ENGINE_SOURCES}
    ${INCLUDE_SOURCES}
)

add_executable(Desktop_Pet ${ALL_SOURCES})

# 设置包含目录
target_include_directories(Desktop_Pet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(Desktop_Pet
        Qt::Core
        Qt::Gui
        Qt::Widgets
)

if (WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(DEBUG_SUFFIX)
    if (MSVC AND CMAKE_BUILD_TYPE MATCHES "Debug")
        set(DEBUG_SUFFIX "d")
    endif ()
    set(QT_INSTALL_PATH "${CMAKE_PREFIX_PATH}")
    if (NOT EXISTS "${QT_INSTALL_PATH}/bin")
        set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
        if (NOT EXISTS "${QT_INSTALL_PATH}/bin")
            set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
        endif ()
    endif ()
    if (EXISTS "${QT_INSTALL_PATH}/plugins/platforms/qwindows${DEBUG_SUFFIX}.dll")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/plugins/platforms/")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                "${QT_INSTALL_PATH}/plugins/platforms/qwindows${DEBUG_SUFFIX}.dll"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/plugins/platforms/")
    endif ()
    foreach (QT_LIB Core Gui Widgets)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}${DEBUG_SUFFIX}.dll"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
    endforeach (QT_LIB)
endif ()


