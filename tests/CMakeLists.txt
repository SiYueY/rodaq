add_executable(rodaq_result_test result_test.cpp)
target_link_libraries(rodaq_result_test PRIVATE rodaq::plugin)
add_test(NAME rodaq_result_test COMMAND rodaq_result_test)

add_executable(rodaq_logging_test logging_test.cpp)
target_link_libraries(rodaq_logging_test PRIVATE rodaq::plugin)
add_test(NAME rodaq_logging_test COMMAND rodaq_logging_test)

add_executable(rodaq_public_headers_test public_headers_test.cpp)
target_link_libraries(rodaq_public_headers_test PRIVATE rodaq::plugin)
add_test(NAME rodaq_public_headers_test COMMAND rodaq_public_headers_test)

add_executable(rodaq_logging_async_file_test logging_async_file_test.cpp)
target_link_libraries(rodaq_logging_async_file_test PRIVATE rodaq::plugin)
add_test(NAME rodaq_logging_async_file_test COMMAND rodaq_logging_async_file_test)

add_executable(rodaq_logging_compile_level_test logging_compile_level_test.cpp)
target_link_libraries(rodaq_logging_compile_level_test PRIVATE rodaq::plugin)
add_test(NAME rodaq_logging_compile_level_test COMMAND rodaq_logging_compile_level_test)

# Tests use assertions even in release builds.
get_property(test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS test_targets)
    target_compile_options(${target} PRIVATE -UNDEBUG)
    rodaq_enable_project_warnings(${target})
endforeach()

foreach(header status result logging)
    set(source ${CMAKE_CURRENT_BINARY_DIR}/header_${header}.cpp)
    file(WRITE ${source} "#include <rodaq/${header}.hpp>\n")
    add_library(rodaq_header_${header} OBJECT ${source})
    target_link_libraries(rodaq_header_${header} PRIVATE rodaq::plugin)
    rodaq_enable_project_warnings(rodaq_header_${header})
endforeach()

find_package(GTest 1.11 REQUIRED)
add_executable(rodaq_build_contract_test build_contract_test.cpp)
target_link_libraries(rodaq_build_contract_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_build_contract_test)
include(GoogleTest)
gtest_discover_tests(rodaq_build_contract_test)
add_test(NAME rodaq_package_contract
    COMMAND ${CMAKE_COMMAND}
        -DRODAQ_BUILD_DIR=${PROJECT_BINARY_DIR}
        -DRODAQ_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DRODAQ_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DRODAQ_SANITIZER=${RODAQ_SANITIZER}
        -P ${PROJECT_SOURCE_DIR}/tests/package_contract.cmake)
