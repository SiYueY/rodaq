add_executable(rodaq_status_test status_test.cpp)
target_link_libraries(rodaq_status_test
    PRIVATE
        GTest::gtest_main
        rodaq_base
)
rodaq_configure_test(rodaq_status_test)
add_test(NAME rodaq.status COMMAND rodaq_status_test)

add_executable(rodaq_result_test result_test.cpp)
target_link_libraries(rodaq_result_test
    PRIVATE
        GTest::gtest_main
        rodaq_base
)
rodaq_configure_test(rodaq_result_test)
add_test(NAME rodaq.result COMMAND rodaq_result_test)
