include_guard(GLOBAL)

option(DESKTOP_PET_DEPLOY_WINDOWS_RUNTIME
    "Deploy Qt plugins and runtime DLLs beside every Windows executable" ON)
set(DESKTOP_PET_RUNTIME_SEARCH_DIRS "" CACHE STRING
    "Additional runtime DLL directories (semicolon-separated, matching the build ABI/configuration)")

if(NOT WIN32 OR NOT DESKTOP_PET_DEPLOY_WINDOWS_RUNTIME)
    return()
endif()
if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Windows runtime deployment requires a native Windows build. "
        "For cross compilation, set DESKTOP_PET_DEPLOY_WINDOWS_RUNTIME=OFF and deploy separately.")
endif()

set(_qt_bin_hints "")
if(TARGET Qt6::qmake)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    if(_qmake)
        get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
        list(APPEND _qt_bin_hints "${_qt_bin}")
    endif()
endif()
if(Qt6Core_DIR)
    get_filename_component(_qt_bin "${Qt6Core_DIR}/../../../bin" ABSOLUTE)
    list(APPEND _qt_bin_hints "${_qt_bin}")
endif()
# Do not silently select windeployqt from an unrelated Qt installation on PATH.
find_program(DESKTOP_PET_WINDEPLOYQT NAMES windeployqt.exe windeployqt
    HINTS ${_qt_bin_hints} NO_DEFAULT_PATH)
if(NOT DESKTOP_PET_WINDEPLOYQT OR NOT EXISTS "${DESKTOP_PET_WINDEPLOYQT}")
    message(FATAL_ERROR "windeployqt was not found in the selected Qt kit. Install its deployment tools "
        "or set DESKTOP_PET_WINDEPLOYQT to the matching executable. "
        "Use DESKTOP_PET_DEPLOY_WINDOWS_RUNTIME=OFF only if you deploy DLLs yourself.")
endif()

get_filename_component(_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(_deploy_bin "${DESKTOP_PET_WINDEPLOYQT}" DIRECTORY)
if(MSVC)
    get_filename_component(_linker_bin "${CMAKE_LINKER}" DIRECTORY)
    find_program(DESKTOP_PET_DUMPBIN NAMES dumpbin.exe dumpbin
        HINTS "${_compiler_bin}" "${_linker_bin}")
    set(_dependency_tool dumpbin)
    set(_dependency_command "${DESKTOP_PET_DUMPBIN}")
else()
    set(_dependency_tool objdump)
    set(_dependency_command "${CMAKE_OBJDUMP}")
endif()
if(NOT _dependency_command OR NOT EXISTS "${_dependency_command}")
    message(FATAL_ERROR "Runtime DLL scanning requires ${_dependency_tool} from the selected compiler kit.")
endif()

set(_runtime_dirs "$<TARGET_FILE_DIR:Qt6::Core>" "${_deploy_bin}" "${_compiler_bin}")
if(SODIUM_LIBRARY)
    get_filename_component(_sodium_libdir "${SODIUM_LIBRARY}" DIRECTORY)
    list(APPEND _runtime_dirs "${_sodium_libdir}" "${_sodium_libdir}/../bin")
endif()
if(DESKTOP_PET_QTKEYCHAIN_TARGET)
    list(APPEND _runtime_dirs
        "$<TARGET_FILE_DIR:${DESKTOP_PET_QTKEYCHAIN_TARGET}>"
        "$<TARGET_FILE_DIR:${DESKTOP_PET_QTKEYCHAIN_TARGET}>/../bin")
endif()
if(ORT_FOUND)
    list(APPEND _runtime_dirs "${ORT_ROOT}/bin" "${ORT_ROOT}/lib")
endif()
# Explicit directories take precedence over inferred third-party locations.
set(_runtime_dirs ${DESKTOP_PET_RUNTIME_SEARCH_DIRS} ${_runtime_dirs})

# Call this module after all executables have been declared, including test fixtures.
get_property(_runtime_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _runtime_targets)
    get_target_property(_type ${_target} TYPE)
    if(NOT _type STREQUAL "EXECUTABLE")
        continue()
    endif()
    # windeployqt rejects plain C++ fixtures; those still need compiler DLL scanning.
    get_target_property(_deploy_qt ${_target} DESKTOP_PET_DEPLOY_QT)
    if(NOT _deploy_qt STREQUAL "OFF")
        set(_deploy_qt ON)
    endif()
    set(_script "${CMAKE_CURRENT_BINARY_DIR}/runtime-deploy/${_target}-$<CONFIG>.cmake")
    file(GENERATE OUTPUT "${_script}" CONTENT
"set(DEPLOY_EXECUTABLE [==[$<TARGET_FILE:${_target}>]==])
set(DEPLOY_CONFIG [==[$<CONFIG>]==])
set(DEPLOY_QT ${_deploy_qt})
set(DEPLOY_WINDEPLOYQT [==[${DESKTOP_PET_WINDEPLOYQT}]==])
set(DEPLOY_SEARCH_DIRS [==[${_runtime_dirs}]==])
set(DEPLOY_EXTRA_DLLS [==[$<TARGET_PROPERTY:${_target},DESKTOP_PET_EXTRA_RUNTIME_DLLS>]==])
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL ${_dependency_tool})
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND [==[${_dependency_command}]==])
include([==[${CMAKE_CURRENT_LIST_DIR}/DeployWindowsRuntime.cmake]==])
")
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -P "${_script}"
        COMMENT "Deploying Windows runtime for ${_target}"
        VERBATIM)
    # Also support repairing a deployment after DLLs were removed without relinking.
    add_custom_target(${_target}_deploy_runtime
        COMMAND "${CMAKE_COMMAND}" -P "${_script}"
        DEPENDS ${_target}
        VERBATIM)
endforeach()
message(STATUS "Windows runtime deployment enabled for all executables: ${DESKTOP_PET_WINDEPLOYQT}")
