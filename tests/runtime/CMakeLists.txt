add_executable(rodaq_projection_registry_test interop_projection_registry_test.cpp)
target_link_libraries(rodaq_projection_registry_test
    PRIVATE
        GTest::gtest_main
        rodaq_runtime
)
rodaq_enable_warnings(rodaq_projection_registry_test)
rodaq_enable_sanitizers(rodaq_projection_registry_test)
add_test(NAME rodaq.runtime.projection_registry COMMAND rodaq_projection_registry_test)

add_executable(rodaq_clock_correlation_test clock_correlation_test.cpp)
target_link_libraries(rodaq_clock_correlation_test
    PRIVATE
        GTest::gtest_main
        rodaq_runtime
)
rodaq_enable_warnings(rodaq_clock_correlation_test)
rodaq_enable_sanitizers(rodaq_clock_correlation_test)
add_test(NAME rodaq.runtime.clock_correlation COMMAND rodaq_clock_correlation_test)
