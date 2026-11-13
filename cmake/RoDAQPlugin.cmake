include_guard(GLOBAL)

set(RODAQ_PLUGIN_PACKAGE_ROOT "${CMAKE_BINARY_DIR}/rodaq_plugins" CACHE INTERNAL "")

function(rodaq_register_plugin target)
    cmake_parse_arguments(
        PARSE_ARGV 1
        plugin
        ""
        "PLUGIN_ID;PLUGIN_NAME;PLUGIN_VERSION"
        "")

    if(NOT TARGET "${target}")
        message(FATAL_ERROR "rodaq_register_plugin requires an existing target")
    endif()
    if(NOT plugin_PLUGIN_ID OR NOT plugin_PLUGIN_NAME)
        message(FATAL_ERROR "rodaq_register_plugin requires PLUGIN_ID and PLUGIN_NAME")
    endif()
    if(NOT plugin_PLUGIN_ID MATCHES "^[a-z0-9._-]+$")
        message(FATAL_ERROR "Invalid RoDAQ plugin id: ${plugin_PLUGIN_ID}")
    endif()

    get_target_property(target_type "${target}" TYPE)
    if(NOT target_type STREQUAL "SHARED_LIBRARY")
        message(FATAL_ERROR "RoDAQ plugins must be SHARED libraries")
    endif()

    if(plugin_PLUGIN_VERSION)
        set(plugin_version "${plugin_PLUGIN_VERSION}")
    elseif(PROJECT_VERSION)
        set(plugin_version "${PROJECT_VERSION}")
    else()
        message(FATAL_ERROR "PLUGIN_VERSION or PROJECT_VERSION is required")
    endif()
    if(NOT plugin_version MATCHES
       "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?$")
        message(FATAL_ERROR "Invalid RoDAQ plugin version: ${plugin_version}")
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
        set(plugin_arch "amd64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(plugin_arch "arm64")
    else()
        message(FATAL_ERROR "Unsupported RoDAQ plugin architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    target_link_libraries("${target}" PRIVATE rodaq_plugin_authoring)
    set_target_properties(
        "${target}"
        PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES)
    target_link_options(
        "${target}"
        PRIVATE
            "LINKER:--version-script=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rodaq_plugin.map"
            "LINKER:--no-undefined")
    set_property(
        TARGET "${target}"
        APPEND
        PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rodaq_plugin.map")
    target_compile_definitions(
        "${target}"
        PRIVATE
            RODAQ_PLUGIN_ID="${plugin_PLUGIN_ID}"
            RODAQ_PLUGIN_NAME="${plugin_PLUGIN_NAME}"
            RODAQ_PLUGIN_VERSION="${plugin_version}")

    set(package_dir "${RODAQ_PLUGIN_PACKAGE_ROOT}/${target}")
    set(package_lib_dir "${package_dir}/lib")
    set(manifest_path "${package_dir}/manifest.json")

    add_custom_command(
        TARGET "${target}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${package_lib_dir}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:${target}>"
            "${package_lib_dir}/$<TARGET_FILE_NAME:${target}>"
        COMMAND "${CMAKE_COMMAND}"
            "-DRODAQ_MANIFEST_OUTPUT=${manifest_path}"
            "-DRODAQ_ARTIFACT_PATH=$<TARGET_FILE:${target}>"
            "-DRODAQ_PLUGIN_ID=${plugin_PLUGIN_ID}"
            "-DRODAQ_PLUGIN_NAME=${plugin_PLUGIN_NAME}"
            "-DRODAQ_PLUGIN_VERSION=${plugin_version}"
            "-DRODAQ_PLUGIN_ARCH=${plugin_arch}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GeneratePluginManifest.cmake"
        COMMAND "${CMAKE_COMMAND}"
            "-DRODAQ_PLUGIN_LIBRARY=$<TARGET_FILE:${target}>"
            -DCMAKE_NM="${CMAKE_NM}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CheckPluginExports.cmake"
        VERBATIM)

    set_target_properties(
        "${target}"
        PROPERTIES
            RODAQ_PLUGIN_ID "${plugin_PLUGIN_ID}"
            RODAQ_PLUGIN_VERSION "${plugin_version}"
            RODAQ_PLUGIN_PACKAGE_DIR "${package_dir}")
endfunction()
