cmake_minimum_required(VERSION 3.20)

if(NOT EXISTS "${DEPLOY_EXECUTABLE}")
    message(FATAL_ERROR "Cannot deploy missing executable: ${DEPLOY_EXECUTABLE}")
endif()
get_filename_component(_destination "${DEPLOY_EXECUTABLE}" DIRECTORY)
# Parallel targets share an output directory. Serialize all runtime/plugin writes.
file(LOCK "${_destination}/.desktop-pet-runtime.lock" GUARD PROCESS TIMEOUT 600)

set(_search_dirs "")
foreach(_dir IN LISTS DEPLOY_SEARCH_DIRS)
    if(IS_DIRECTORY "${_dir}")
        get_filename_component(_dir "${_dir}" ABSOLUTE)
        list(APPEND _search_dirs "${_dir}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _search_dirs)
# windeployqt itself needs the matching Qt and compiler runtime on PATH.
set(ENV{PATH} "${_search_dirs};$ENV{PATH}")

if(DEPLOY_CONFIG STREQUAL "Debug")
    set(_qt_configuration --debug)
else()
    set(_qt_configuration --release)
endif()
if(DEPLOY_QT)
    execute_process(COMMAND "${DEPLOY_WINDEPLOYQT}"
        ${_qt_configuration} --compiler-runtime --no-translations
        --dir "${_destination}" "${DEPLOY_EXECUTABLE}"
        RESULT_VARIABLE _deploy_result OUTPUT_VARIABLE _deploy_output ERROR_VARIABLE _deploy_error)
    if(NOT _deploy_result STREQUAL "0")
        message(FATAL_ERROR "windeployqt failed for ${DEPLOY_EXECUTABLE}:\n${_deploy_output}\n${_deploy_error}")
    endif()
endif()

# ONNX provider DLLs may be loaded dynamically rather than appear in the import table.
foreach(_dll IN LISTS DEPLOY_EXTRA_DLLS)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dll}" "${_destination}"
        RESULT_VARIABLE _copy_result)
    if(NOT _copy_result STREQUAL "0")
        message(FATAL_ERROR "Failed to deploy ${_dll}. Stop running pets/tests and retry the build.")
    endif()
endforeach()

# Scan required dynamically loaded plugins too, not optional SQL drivers (e.g. MySQL).
set(_plugins "")
if(DEPLOY_QT)
    file(GLOB _plugins
        "${_destination}/platforms/*.dll"
        "${_destination}/sqldrivers/qsqlite*.dll"
        "${_destination}/tls/*.dll"
        "${_destination}/imageformats/*.dll")
endif()
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${DEPLOY_EXECUTABLE}"
    LIBRARIES ${_plugins} ${DEPLOY_EXTRA_DLLS}
    DIRECTORIES ${_search_dirs}
    RESOLVED_DEPENDENCIES_VAR _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-")
if(_unresolved)
    message(FATAL_ERROR "Missing runtime DLLs for ${DEPLOY_EXECUTABLE}: ${_unresolved}. "
        "Install matching architecture/configuration dependencies or set "
        "DESKTOP_PET_RUNTIME_SEARCH_DIRS to their bin directories, then rebuild. "
        "Do not download individual DLLs from unknown sites.")
endif()
file(TO_CMAKE_PATH "$ENV{SystemRoot}" _system_root)
string(TOLOWER "${_system_root}/" _system_prefix)
foreach(_dll IN LISTS _resolved)
    file(TO_CMAKE_PATH "${_dll}" _normalized)
    string(TOLOWER "${_normalized}" _lower)
    string(FIND "${_lower}" "${_system_prefix}" _system_index)
    if(_system_root AND _system_index EQUAL 0)
        continue()
    endif()
    get_filename_component(_dll_dir "${_dll}" DIRECTORY)
    if(_dll_dir STREQUAL _destination)
        continue()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dll}" "${_destination}"
        RESULT_VARIABLE _copy_result)
    if(NOT _copy_result STREQUAL "0")
        message(FATAL_ERROR "Failed to deploy ${_dll}. Stop running pets/tests and retry the build.")
    endif()
endforeach()
