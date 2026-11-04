add_library(rodaq_fake_acquisition_plugin SHARED fake_acquisition_plugin.cpp)
target_link_libraries(rodaq_fake_acquisition_plugin PRIVATE rodaq::sdk)
rodaq_enable_warnings(rodaq_fake_acquisition_plugin)
rodaq_enable_sanitizers(rodaq_fake_acquisition_plugin)
rodaq_configure_plugin(
    TARGET rodaq_fake_acquisition_plugin
    FAMILY acquisition
    MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/fake_acquisition_plugin.json"
)

add_library(rodaq_fake_storage_plugin SHARED fake_storage_plugin.cpp)
target_link_libraries(rodaq_fake_storage_plugin PRIVATE rodaq::sdk)
rodaq_enable_warnings(rodaq_fake_storage_plugin)
rodaq_enable_sanitizers(rodaq_fake_storage_plugin)
rodaq_configure_plugin(
    TARGET rodaq_fake_storage_plugin
    FAMILY storage
    MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/fake_storage_plugin.json"
)

add_executable(rodaq_runtime_pipeline_test runtime_pipeline_test.cpp)
target_link_libraries(rodaq_runtime_pipeline_test
    PRIVATE
        GTest::gtest_main
        rodaq_runtime_testing
)
rodaq_enable_warnings(rodaq_runtime_pipeline_test)
rodaq_enable_sanitizers(rodaq_runtime_pipeline_test)
add_dependencies(
    rodaq_runtime_pipeline_test
    rodaq_fake_acquisition_plugin
    rodaq_fake_storage_plugin
)
add_test(
    NAME rodaq.integration.runtime_pipeline
    COMMAND rodaq_runtime_pipeline_test
)
set_tests_properties(
    rodaq.integration.runtime_pipeline
    PROPERTIES
        ENVIRONMENT
            "RODAQ_FAKE_ACQUISITION_PLUGIN=$<TARGET_FILE:rodaq_fake_acquisition_plugin>;RODAQ_FAKE_STORAGE_PLUGIN=$<TARGET_FILE:rodaq_fake_storage_plugin>;TSAN_OPTIONS=report_thread_leaks=0"
)
