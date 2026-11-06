function(rodaq_configure_test target)
    rodaq_enable_warnings("${target}")
    rodaq_enable_sanitizers("${target}")
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

add_executable(rodaq_status_test unit/status_test.cpp)
target_link_libraries(rodaq_status_test
    PRIVATE
        GTest::gtest_main
        rodaq_foundation
)
rodaq_configure_test(rodaq_status_test)
add_test(
    NAME rodaq.status
    COMMAND rodaq_status_test
)

add_executable(rodaq_result_test unit/result_test.cpp)
target_link_libraries(rodaq_result_test
    PRIVATE
        GTest::gtest_main
        rodaq_foundation
)
rodaq_configure_test(rodaq_result_test)
add_test(
    NAME rodaq.result
    COMMAND rodaq_result_test
)

add_subdirectory(abi)
add_subdirectory(plugin)
add_subdirectory(runtime)
add_subdirectory(integration)

add_test(
    NAME rodaq.cmake.plugin_manifest_matrix
    COMMAND
        "${CMAKE_COMMAND}"
        "-DRODAQ_HELPER=${PROJECT_SOURCE_DIR}/cmake/rodaq_plugin.cmake"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/plugin_manifest/run_matrix.cmake"
)
