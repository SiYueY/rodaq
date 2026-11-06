function(rodaq_configure_test target)
    rodaq_enable_warnings("${target}")
    rodaq_enable_sanitizers("${target}")
endfunction()

function(rodaq_add_test target test_name source)
    add_executable("${target}" "${source}")
    target_link_libraries("${target}" PRIVATE GTest::gtest_main rodaq_testing)
    rodaq_configure_test("${target}")
    add_test(NAME "rodaq.${test_name}" COMMAND "${target}")
endfunction()

add_executable(rodaq_status_header_test compile/status_header_test.cpp)
target_link_libraries(rodaq_status_header_test PRIVATE rodaq_public_api)
rodaq_configure_test(rodaq_status_header_test)
add_test(
    NAME rodaq.compile.status_header
    COMMAND rodaq_status_header_test
)

add_executable(rodaq_result_header_test compile/result_header_test.cpp)
target_link_libraries(rodaq_result_header_test PRIVATE rodaq_public_api)
rodaq_configure_test(rodaq_result_header_test)
add_test(
    NAME rodaq.compile.result_header
    COMMAND rodaq_result_header_test
)

add_executable(rodaq_plugin_sdk_header_test compile/plugin_sdk_header_test.cpp)
target_link_libraries(rodaq_plugin_sdk_header_test PRIVATE rodaq::sdk)
rodaq_configure_test(rodaq_plugin_sdk_header_test)
add_test(
    NAME rodaq.compile.plugin_sdk_header
    COMMAND rodaq_plugin_sdk_header_test
)

add_subdirectory(abi)
add_subdirectory(plugin)
add_subdirectory(activation)
add_subdirectory(daq)
add_subdirectory(admission)
add_subdirectory(binding)
add_subdirectory(canonical)
add_subdirectory(clock)
add_subdirectory(config)
add_subdirectory(logging)
add_subdirectory(metrics)
add_subdirectory(payload)
add_subdirectory(projection)
add_subdirectory(record)
add_subdirectory(storage)
add_subdirectory(integration)

add_test(
    NAME rodaq.compile.core_naming_residue
    COMMAND
        "${CMAKE_COMMAND}"
        "-DRODAQ_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_core_naming_residue.cmake"
)

add_test(
    NAME rodaq.cmake.plugin_manifest_matrix
    COMMAND
        "${CMAKE_COMMAND}"
        "-DRODAQ_HELPER=${PROJECT_SOURCE_DIR}/cmake/rodaq_plugin.cmake"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/plugin_manifest/run_matrix.cmake"
)
