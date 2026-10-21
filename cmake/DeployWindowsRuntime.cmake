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

if(DEPLOY_QT)
    execute_process(COMMAND "${DEPLOY_WINDEPLOYQT}"
        --compiler-runtime --no-translations
        --dir "${_destination}" "${DEPLOY_EXECUTABLE}"
        RESULT_VARIABLE _deploy_result OUTPUT_VARIABLE _deploy_output ERROR_VARIABLE _deploy_error)
    if(NOT _deploy_result STREQUAL "0")
        message(FATAL_ERROR "windeployqt failed for ${DEPLOY_EXECUTABLE}:\n${_deploy_output}\n${_deploy_error}")
    endif()
    # Schannel provides the supported Windows TLS backend. Do not ship the
    # optional OpenSSL backend without its matching OpenSSL runtime, and clean
    # the duplicate platform layout produced by older deployment attempts.
    file(REMOVE "${_destination}/tls/qopensslbackend.dll")
    file(REMOVE_RECURSE "${_destination}/plugins/platforms")

    # windeployqt selects plugins from the actual executable. A target that
    # links Qt Sql can add qsqlite here; targets without Qt Sql must preserve a
    # shared qsqlite deployment. Remove only drivers with vendor dependencies.
    file(GLOB _sql_plugins "${_destination}/sqldrivers/*")
    foreach(_plugin IN LISTS _sql_plugins)
        get_filename_component(_plugin_name "${_plugin}" NAME)
        string(TOLOWER "${_plugin_name}" _plugin_name_lower)
        if(NOT _plugin_name_lower MATCHES "^qsqlite(d)?\\.dll$")
            file(REMOVE_RECURSE "${_plugin}")
        endif()
    endforeach()
endif()

# ONNX provider DLLs may be loaded dynamically rather than appear in the import table.
set(_deployed_extra_dlls "")
foreach(_dll IN LISTS DEPLOY_EXTRA_DLLS)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dll}" "${_destination}"
        RESULT_VARIABLE _copy_result)
    if(NOT _copy_result STREQUAL "0")
        message(FATAL_ERROR "Failed to deploy ${_dll}. Stop running pets/tests and retry the build.")
    endif()
    get_filename_component(_dll_name "${_dll}" NAME)
    list(APPEND _deployed_extra_dlls "${_destination}/${_dll_name}")
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
set(_dependency_search_dirs "${_destination}" ${_search_dirs})
list(REMOVE_DUPLICATES _dependency_search_dirs)
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${DEPLOY_EXECUTABLE}"
    LIBRARIES ${_plugins} ${_deployed_extra_dlls}
    DIRECTORIES ${_dependency_search_dirs}
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

function(_desktop_pet_read_pe_machine _path _output)
    set(_machine "")
    if(EXISTS "${_path}")
        file(SIZE "${_path}" _file_size)
        if(_file_size GREATER_EQUAL 64)
            file(READ "${_path}" _dos_magic OFFSET 0 LIMIT 2 HEX)
            file(READ "${_path}" _pe_offset_hex OFFSET 60 LIMIT 4 HEX)
            string(TOLOWER "${_dos_magic}" _dos_magic)
            string(LENGTH "${_pe_offset_hex}" _offset_length)
            if(_dos_magic STREQUAL "4d5a" AND _offset_length EQUAL 8)
                string(SUBSTRING "${_pe_offset_hex}" 0 2 _offset_0)
                string(SUBSTRING "${_pe_offset_hex}" 2 2 _offset_1)
                string(SUBSTRING "${_pe_offset_hex}" 4 2 _offset_2)
                string(SUBSTRING "${_pe_offset_hex}" 6 2 _offset_3)
                math(EXPR _pe_offset "0x${_offset_3}${_offset_2}${_offset_1}${_offset_0}")
                math(EXPR _minimum_size "${_pe_offset} + 6")
                if(_file_size GREATER_EQUAL _minimum_size)
                    file(READ "${_path}" _pe_signature OFFSET ${_pe_offset} LIMIT 4 HEX)
                    math(EXPR _machine_offset "${_pe_offset} + 4")
                    file(READ "${_path}" _machine OFFSET ${_machine_offset} LIMIT 2 HEX)
                    string(TOLOWER "${_pe_signature}" _pe_signature)
                    string(TOLOWER "${_machine}" _machine)
                    if(NOT _pe_signature STREQUAL "50450000")
                        set(_machine "")
                    endif()
                endif()
            endif()
        endif()
    endif()
    set(${_output} "${_machine}" PARENT_SCOPE)
endfunction()

_desktop_pet_read_pe_machine("${DEPLOY_EXECUTABLE}" _executable_machine)
if(NOT _executable_machine)
    message(FATAL_ERROR "Cannot determine the PE architecture of ${DEPLOY_EXECUTABLE}.")
endif()
foreach(_dll IN LISTS _resolved)
    file(TO_CMAKE_PATH "${_dll}" _normalized)
    string(TOLOWER "${_normalized}" _lower)
    string(FIND "${_lower}" "${_system_prefix}" _system_index)
    get_filename_component(_dll_name "${_dll}" NAME)
    string(TOLOWER "${_dll_name}" _dll_name_lower)
    # Prebuilt libraries such as ONNX Runtime can use the MSVC runtime even in a
    # MinGW application. Keep the known redistributable v14 DLLs app-local.
    set(_deploy_system_runtime FALSE)
    if(_dll_name_lower MATCHES
       "^(concrt140|msvcp140|msvcp140_1|msvcp140_2|msvcp140_atomic_wait|msvcp140_codecvt_ids|vcamp140|vccorlib140|vcomp140|vcruntime140|vcruntime140_1|vcruntime140_threads)\\.dll$")
        set(_deploy_system_runtime TRUE)
    endif()
    if(_system_root AND _system_index EQUAL 0 AND NOT _deploy_system_runtime)
        continue()
    endif()
    if(_deploy_system_runtime)
        _desktop_pet_read_pe_machine("${_dll}" _runtime_machine)
        if(NOT _runtime_machine)
            message(FATAL_ERROR "Cannot determine the PE architecture of MSVC runtime ${_dll}.")
        endif()
        if(NOT _runtime_machine STREQUAL _executable_machine)
            message(FATAL_ERROR "Refusing to deploy ${_dll}: PE architecture ${_runtime_machine} "
                "does not match executable architecture ${_executable_machine}. Install the "
                "matching VC++ 2015-2022 Redistributable or add its directory to "
                "DESKTOP_PET_RUNTIME_SEARCH_DIRS.")
        endif()
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
