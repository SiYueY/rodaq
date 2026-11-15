function(rodaq_register_plugin target)
    cmake_parse_arguments(PARSE_ARGV 1 plugin "" "PLUGIN_ID;PLUGIN_NAME;PLUGIN_VERSION" "")
    if(plugin_UNPARSED_ARGUMENTS OR plugin_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "rodaq_register_plugin: unknown argument or missing value")
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR "rodaq_register_plugin requires an existing target")
    endif()
    get_target_property(type ${target} TYPE)
    get_target_property(registered ${target} RODAQ_PLUGIN_REGISTERED)
    if(NOT type STREQUAL "SHARED_LIBRARY" OR registered)
        message(FATAL_ERROR "rodaq_register_plugin requires an unregistered SHARED target")
    endif()
    if(NOT plugin_PLUGIN_VERSION)
        set(plugin_PLUGIN_VERSION "${PROJECT_VERSION}")
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "RoDAQ Plugin V1 supports Linux ELF only")
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        set(arch amd64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(arch arm64)
    else()
        message(FATAL_ERROR "Unsupported RoDAQ Plugin architecture")
    endif()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(tool "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/plugin_metadata.py")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/rodaq-generated/${target}")
    execute_process(COMMAND ${Python3_EXECUTABLE} "${tool}" configure
        --id "${plugin_PLUGIN_ID}" --name "${plugin_PLUGIN_NAME}"
        --version "${plugin_PLUGIN_VERSION}" --arch ${arch} --output "${generated}"
        RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Invalid RoDAQ Plugin metadata: ${error}")
    endif()
    set(package_id "${plugin_PLUGIN_ID}")
    if(package_id STREQUAL "." OR package_id STREQUAL "..")
        string(SHA256 package_id "${plugin_PLUGIN_ID}")
    endif()
    set(package "${CMAKE_CURRENT_BINARY_DIR}/rodaq-plugins/${package_id}/${plugin_PLUGIN_VERSION}")
    if(NOT CMAKE_C_COMPILER_LOADED)
        set_source_files_properties("${generated}/descriptor.c" PROPERTIES LANGUAGE CXX)
    endif()
    target_sources(${target} PRIVATE "${generated}/descriptor.c")
    # Pure C authors only require the C headers; C++ authors also get SDK support.
    target_include_directories(${target} PRIVATE $<TARGET_PROPERTY:rodaq::plugin,INTERFACE_INCLUDE_DIRECTORIES>)
    if(CMAKE_CXX_COMPILER_LOADED)
        target_link_libraries(${target} PRIVATE rodaq::plugin)
    endif()
    set_target_properties(${target} PROPERTIES RODAQ_PLUGIN_REGISTERED TRUE
        LIBRARY_OUTPUT_DIRECTORY "${package}/lib"
        C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_link_options(${target} PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/plugin.exports")
    # Clang sanitizer instrumentation intentionally resolves its DSO imports from
    # the instrumented executable. Normal production builds remain fully linked.
    get_target_property(plugin_link_options ${target} LINK_OPTIONS)
    string(JOIN " " plugin_link_flags ${plugin_link_options})
    if(NOT ((CMAKE_C_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        AND (CMAKE_C_FLAGS MATCHES "-fsanitize=" OR CMAKE_CXX_FLAGS MATCHES "-fsanitize="
            OR plugin_link_flags MATCHES "-fsanitize=")))
        target_link_options(${target} PRIVATE "LINKER:--no-undefined")
    endif()
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/plugin.exports")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} "${tool}" manifest
            --metadata "${generated}/metadata.json" --artifact $<TARGET_FILE:${target}>
            --output "${package}/manifest.json" VERBATIM)
    set(destination "lib/rodaq/plugins/${package_id}/${plugin_PLUGIN_VERSION}")
    install(TARGETS ${target} LIBRARY DESTINATION "${destination}/lib")
    # install may edit RPATH or strip the ELF. Hash the final installed bytes,
    # never copy a build-tree digest over a different installed artifact.
    install(CODE "
        execute_process(COMMAND \"${Python3_EXECUTABLE}\" \"${tool}\" manifest
            --metadata \"${generated}/metadata.json\"
            --artifact \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${destination}/lib/$<TARGET_FILE_NAME:${target}>\"
            --output \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${destination}/manifest.json\"
            RESULT_VARIABLE manifest_result)
        if(NOT manifest_result EQUAL 0)
            message(FATAL_ERROR \"Cannot generate installed Plugin manifest\")
        endif()
    ")
endfunction()
