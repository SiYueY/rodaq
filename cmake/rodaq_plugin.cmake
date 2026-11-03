include_guard(GLOBAL)
include(GNUInstallDirs)

function(_rodaq_cpp_escape input output)
    string(REPLACE "\\" "\\\\" escaped "${input}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    set("${output}" "${escaped}" PARENT_SCOPE)
endfunction()

function(_rodaq_validate_manifest manifest family out_id)
    if(NOT EXISTS "${manifest}")
        message(FATAL_ERROR "Plugin manifest does not exist: ${manifest}")
    endif()

    file(READ "${manifest}" json)
    string(JSON id ERROR_VARIABLE json_error GET "${json}" id)
    if(json_error OR id STREQUAL "")
        message(FATAL_ERROR "Plugin manifest requires a non-empty id")
    endif()
    if(NOT id MATCHES "^[a-z0-9_-]+(\\.[a-z0-9_-]+)+$")
        message(FATAL_ERROR "Plugin manifest id does not match the Runtime grammar")
    endif()
    string(JSON name ERROR_VARIABLE json_error GET "${json}" name)
    if(json_error OR name STREQUAL "")
        message(FATAL_ERROR "Plugin manifest requires a non-empty name")
    endif()
    string(JSON version ERROR_VARIABLE json_error GET "${json}" version)
    _rodaq_validate_semver("${version}" valid_version)
    if(json_error OR NOT valid_version)
        message(FATAL_ERROR "Plugin manifest version is not SemVer 2.0")
    endif()
    string(JSON manifest_family ERROR_VARIABLE json_error GET "${json}" family)
    if(json_error OR NOT manifest_family STREQUAL family)
        message(FATAL_ERROR
            "Plugin manifest family '${manifest_family}' does not match '${family}'")
    endif()
    string(JSON description ERROR_VARIABLE json_error GET "${json}" description)
    if(json_error)
        set(description "")
    endif()
    set("${out_id}" "${id}" PARENT_SCOPE)
endfunction()

function(_rodaq_validate_semver_identifiers value reject_numeric_leading_zero out_valid)
    if(value STREQUAL "")
        set("${out_valid}" FALSE PARENT_SCOPE)
        return()
    endif()
    if(NOT value MATCHES "^[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*$")
        set("${out_valid}" FALSE PARENT_SCOPE)
        return()
    endif()
    if(reject_numeric_leading_zero)
        string(REPLACE "." ";" identifiers "${value}")
        foreach(identifier IN LISTS identifiers)
            if(identifier MATCHES "^[0-9]+$" AND identifier MATCHES "^0[0-9]+$")
                set("${out_valid}" FALSE PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endif()
    set("${out_valid}" TRUE PARENT_SCOPE)
endfunction()

function(_rodaq_validate_semver version out_valid)
    set(without_build "${version}")
    string(FIND "${version}" "+" build_separator)
    if(NOT build_separator EQUAL -1)
        string(SUBSTRING "${version}" 0 "${build_separator}" without_build)
        math(EXPR build_start "${build_separator} + 1")
        string(SUBSTRING "${version}" "${build_start}" -1 build)
        if(build MATCHES "\\+" OR NOT build MATCHES "^[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*$")
            set("${out_valid}" FALSE PARENT_SCOPE)
            return()
        endif()
    endif()

    set(core "${without_build}")
    string(FIND "${without_build}" "-" prerelease_separator)
    if(NOT prerelease_separator EQUAL -1)
        string(SUBSTRING "${without_build}" 0 "${prerelease_separator}" core)
        math(EXPR prerelease_start "${prerelease_separator} + 1")
        string(SUBSTRING "${without_build}" "${prerelease_start}" -1 prerelease)
        _rodaq_validate_semver_identifiers("${prerelease}" TRUE prerelease_valid)
        if(NOT prerelease_valid)
            set("${out_valid}" FALSE PARENT_SCOPE)
            return()
        endif()
    endif()

    if(NOT core MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        set("${out_valid}" FALSE PARENT_SCOPE)
        return()
    endif()
    string(REPLACE "." ";" core_parts "${core}")
    list(LENGTH core_parts core_part_count)
    if(NOT core_part_count EQUAL 3)
        set("${out_valid}" FALSE PARENT_SCOPE)
        return()
    endif()
    foreach(part IN LISTS core_parts)
        if(part MATCHES "^0[0-9]+$")
            set("${out_valid}" FALSE PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set("${out_valid}" TRUE PARENT_SCOPE)
endfunction()

function(rodaq_configure_plugin)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "TARGET;FAMILY;MANIFEST" "")
    foreach(required IN ITEMS TARGET FAMILY MANIFEST)
        if(NOT ARG_${required})
            message(FATAL_ERROR "rodaq_configure_plugin requires ${required}")
        endif()
    endforeach()
    if(NOT TARGET "${ARG_TARGET}")
        message(FATAL_ERROR "Unknown Plugin target: ${ARG_TARGET}")
    endif()
    if(NOT ARG_FAMILY MATCHES "^(acquisition|storage)$")
        message(FATAL_ERROR "Plugin FAMILY must be acquisition or storage")
    endif()
    if(NOT TARGET rodaq::sdk)
        message(FATAL_ERROR "rodaq::sdk target is unavailable; find_package(RoDAQ) first")
    endif()

    get_filename_component(manifest "${ARG_MANIFEST}" ABSOLUTE)
    _rodaq_validate_manifest("${manifest}" "${ARG_FAMILY}" plugin_id)
    file(READ "${manifest}" json)
    string(JSON name GET "${json}" name)
    string(JSON version GET "${json}" version)
    string(JSON description ERROR_VARIABLE json_error GET "${json}" description)
    if(json_error)
        set(description "")
    endif()
    _rodaq_cpp_escape("${plugin_id}" plugin_id)
    _rodaq_cpp_escape("${name}" name)
    _rodaq_cpp_escape("${version}" version)
    _rodaq_cpp_escape("${description}" description)

    string(MAKE_C_IDENTIFIER "${ARG_TARGET}" metadata_namespace)
    set(RODAQ_METADATA_NAMESPACE "${metadata_namespace}")
    set(RODAQ_METADATA_ID "${plugin_id}")
    set(RODAQ_METADATA_NAME "${name}")
    set(RODAQ_METADATA_VERSION "${version}")
    set(RODAQ_METADATA_DESCRIPTION "${description}")
    set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/rodaq_plugin_metadata")
    file(MAKE_DIRECTORY "${generated_dir}")
    configure_file(
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rodaq_plugin_metadata.hpp.in"
        "${generated_dir}/${ARG_TARGET}_metadata.hpp"
        @ONLY
    )

    target_compile_features("${ARG_TARGET}" PRIVATE cxx_std_17)
    target_link_libraries("${ARG_TARGET}" PRIVATE rodaq::sdk)
    target_include_directories("${ARG_TARGET}" PRIVATE "${generated_dir}")
    set_target_properties("${ARG_TARGET}" PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        INSTALL_RPATH ""
    )

    if(ARG_FAMILY STREQUAL "acquisition")
        set(entrypoint rodaq_get_acquisition_plugin)
    else()
        set(entrypoint rodaq_get_storage_plugin)
    endif()
    set(version_script "${generated_dir}/${ARG_TARGET}.map")
    file(WRITE "${version_script}"
        "{\n  global:\n    rodaq_plugin_abi_version;\n    ${entrypoint};\n  local:\n    *;\n};\n")
    target_link_options("${ARG_TARGET}" PRIVATE
        "LINKER:--version-script=${version_script}"
        "LINKER:--no-undefined"
    )

    add_custom_command(TARGET "${ARG_TARGET}" POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DRODAQ_TEST_NM=${CMAKE_NM}"
            "-DRODAQ_TEST_READELF=${CMAKE_READELF}"
            "-DRODAQ_TEST_LIBRARY=$<TARGET_FILE:${ARG_TARGET}>"
            "-DRODAQ_EXPECTED_SYMBOLS=rodaq_plugin_abi_version,${entrypoint}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_plugin_symbols.cmake"
        VERBATIM
    )

    install(TARGETS "${ARG_TARGET}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/rodaq/plugins"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}/rodaq/plugins"
    )
    install(FILES "${manifest}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/rodaq/plugins"
        RENAME "${ARG_TARGET}.json"
    )
endfunction()
