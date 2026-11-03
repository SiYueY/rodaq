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

add_executable(rodaq_handle_registry_test handle_registry_test.cpp)
target_link_libraries(rodaq_handle_registry_test
    PRIVATE
        GTest::gtest_main
        rodaq_runtime
)
rodaq_enable_warnings(rodaq_handle_registry_test)
rodaq_enable_sanitizers(rodaq_handle_registry_test)
add_test(NAME rodaq.runtime.handle_registry COMMAND rodaq_handle_registry_test)

add_executable(rodaq_structural_arena_test structural_arena_test.cpp)
target_link_libraries(rodaq_structural_arena_test
    PRIVATE
        GTest::gtest_main
        rodaq_runtime
)
rodaq_enable_warnings(rodaq_structural_arena_test)
rodaq_enable_sanitizers(rodaq_structural_arena_test)
add_test(NAME rodaq.runtime.structural_arena COMMAND rodaq_structural_arena_test)

add_executable(rodaq_payload_pool_test payload_pool_test.cpp)
target_link_libraries(rodaq_payload_pool_test
    PRIVATE
        GTest::gtest_main
        rodaq_runtime
)
rodaq_enable_warnings(rodaq_payload_pool_test)
rodaq_enable_sanitizers(rodaq_payload_pool_test)
add_test(NAME rodaq.runtime.payload_pool COMMAND rodaq_payload_pool_test)

add_executable(rodaq_resource_budget_test resource_budget_test.cpp)
target_link_libraries(rodaq_resource_budget_test
    PRIVATE
        GTest::gtest_main
        rodaq_runtime
)
rodaq_enable_warnings(rodaq_resource_budget_test)
rodaq_enable_sanitizers(rodaq_resource_budget_test)
add_test(NAME rodaq.runtime.resource_budget COMMAND rodaq_resource_budget_test)
