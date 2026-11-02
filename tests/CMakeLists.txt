function(rodaq_configure_test target)
    rodaq_enable_warnings("${target}")
    rodaq_enable_sanitizers("${target}")
endfunction()

add_executable(rodaq_error_header_test compile/error_header_test.cpp)
target_link_libraries(rodaq_error_header_test PRIVATE rodaq_public_api)
rodaq_configure_test(rodaq_error_header_test)
add_test(
    NAME rodaq.compile.error_header
    COMMAND rodaq_error_header_test
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

add_executable(rodaq_error_test unit/error_test.cpp)
target_link_libraries(rodaq_error_test
    PRIVATE
        GTest::gtest_main
        rodaq_foundation
)
rodaq_configure_test(rodaq_error_test)
add_test(
    NAME rodaq.error
    COMMAND rodaq_error_test
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
