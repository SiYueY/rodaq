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

add_executable(rodaq_daq_pipeline_test daq_pipeline_test.cpp)
target_link_libraries(rodaq_daq_pipeline_test
    PRIVATE
        GTest::gtest_main
        rodaq_testing
)
rodaq_enable_warnings(rodaq_daq_pipeline_test)
rodaq_enable_sanitizers(rodaq_daq_pipeline_test)
add_dependencies(
    rodaq_daq_pipeline_test
    rodaq_fake_acquisition_plugin
    rodaq_fake_storage_plugin
)
add_test(
    NAME rodaq.integration.daq_pipeline
    COMMAND rodaq_daq_pipeline_test
)
set_tests_properties(
    rodaq.integration.daq_pipeline
    PROPERTIES
        ENVIRONMENT
            "RODAQ_FAKE_ACQUISITION_PLUGIN=$<TARGET_FILE:rodaq_fake_acquisition_plugin>;RODAQ_FAKE_STORAGE_PLUGIN=$<TARGET_FILE:rodaq_fake_storage_plugin>"
)

rodaq_add_test(rodaq_m1_integration_test integration.m1 m1_integration_test.cpp)
