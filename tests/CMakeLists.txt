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

file(GLOB_RECURSE public_cpp_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/rodaq/*.hpp)
foreach(header IN LISTS public_cpp_headers)
    file(RELATIVE_PATH include_path ${PROJECT_SOURCE_DIR}/include ${header})
    string(MAKE_C_IDENTIFIER "${include_path}" header_id)
    set(source ${CMAKE_CURRENT_BINARY_DIR}/${header_id}.cpp)
    file(GENERATE OUTPUT ${source} CONTENT "#include <${include_path}>\n")
    add_library(${header_id} OBJECT ${source})
    target_link_libraries(${header_id} PRIVATE rodaq::plugin)
    rodaq_enable_project_warnings(${header_id})
endforeach()

find_package(GTest 1.11 REQUIRED)
add_executable(rodaq_build_contract_test build_contract_test.cpp)
target_link_libraries(rodaq_build_contract_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_build_contract_test)
include(GoogleTest)
gtest_discover_tests(rodaq_build_contract_test)
add_executable(rodaq_abi_boundary_test plugin/abi_boundary_test.cpp)
target_link_libraries(rodaq_abi_boundary_test PRIVATE rodaq::plugin GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_abi_boundary_test)
gtest_discover_tests(rodaq_abi_boundary_test)
add_executable(rodaq_plugin_authoring_test plugin/authoring_test.cpp)
target_link_libraries(rodaq_plugin_authoring_test PRIVATE rodaq::plugin GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_plugin_authoring_test)
gtest_discover_tests(rodaq_plugin_authoring_test)
add_executable(rodaq_plugin_catalog_test plugin/catalog_test.cpp)
target_include_directories(rodaq_plugin_catalog_test PRIVATE ${PROJECT_SOURCE_DIR}/src/plugin)
target_link_libraries(rodaq_plugin_catalog_test PRIVATE rodaq::plugin GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_plugin_catalog_test)
gtest_discover_tests(rodaq_plugin_catalog_test)

# Compile every C ABI header alone in both supported language modes.
file(GLOB_RECURSE abi_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/rodaq/*/c/*.h)
foreach(header IN LISTS abi_headers)
    file(RELATIVE_PATH include_path ${PROJECT_SOURCE_DIR}/include ${header})
    string(MAKE_C_IDENTIFIER "${include_path}" header_id)
    foreach(language c cpp)
        set(source ${CMAKE_CURRENT_BINARY_DIR}/${header_id}.${language})
        file(GENERATE OUTPUT ${source} CONTENT "#include <${include_path}>\n")
        add_library(${header_id}_${language} OBJECT ${source})
        target_include_directories(${header_id}_${language} PRIVATE ${PROJECT_SOURCE_DIR}/include)
        rodaq_enable_project_warnings(${header_id}_${language})
    endforeach()
endforeach()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
string(TOUPPER "${CMAKE_BUILD_TYPE}" rodaq_build_type)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/abi_probe.c
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/generate_abi_probe.py
        --root ${PROJECT_SOURCE_DIR} --output ${CMAKE_CURRENT_BINARY_DIR}/abi_probe.c
        "--flags=${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${rodaq_build_type}} $<JOIN:$<TARGET_PROPERTY:rodaq_abi_probe,COMPILE_OPTIONS>, > -std=c11; position_independent=${CMAKE_POSITION_INDEPENDENT_CODE}"
    DEPENDS ${abi_headers} ${PROJECT_SOURCE_DIR}/tools/generate_abi_probe.py
    VERBATIM)
add_executable(rodaq_abi_probe ${CMAKE_CURRENT_BINARY_DIR}/abi_probe.c)
target_include_directories(rodaq_abi_probe PRIVATE ${PROJECT_SOURCE_DIR}/include)
rodaq_enable_project_warnings(rodaq_abi_probe)
add_library(rodaq_abi_signatures OBJECT abi_signatures.c)
target_include_directories(rodaq_abi_signatures PRIVATE ${PROJECT_SOURCE_DIR}/include)
rodaq_enable_project_warnings(rodaq_abi_signatures)
configure_file(abi_signatures.c ${CMAKE_CURRENT_BINARY_DIR}/abi_signatures.cpp COPYONLY)
add_library(rodaq_abi_signatures_cpp OBJECT ${CMAKE_CURRENT_BINARY_DIR}/abi_signatures.cpp)
target_include_directories(rodaq_abi_signatures_cpp PRIVATE ${PROJECT_SOURCE_DIR}/include)
rodaq_enable_project_warnings(rodaq_abi_signatures_cpp)
add_test(NAME rodaq_abi_manifest_comparator
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/abi_manifest_test.py)
add_test(NAME rodaq_plugin_metadata
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/plugin_metadata_test.py)
add_test(NAME rodaq_counter_binary_matrix
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/plugin/abi/run_counter_matrix.py
        --source ${PROJECT_SOURCE_DIR} --build ${PROJECT_BINARY_DIR}
        --compiler ${CMAKE_C_COMPILER} --cxx-compiler ${CMAKE_CXX_COMPILER})

add_test(NAME rodaq_package_contract
    COMMAND ${CMAKE_COMMAND}
        -DRODAQ_BUILD_DIR=${PROJECT_BINARY_DIR}
        -DRODAQ_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DRODAQ_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DRODAQ_SANITIZER=${RODAQ_SANITIZER}
        -P ${PROJECT_SOURCE_DIR}/tests/package_contract.cmake)
