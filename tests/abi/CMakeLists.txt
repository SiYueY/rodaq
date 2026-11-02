set(RODAQ_ABI_HEADER_NAMES
    common
    canonical
    service
    config
    runtime
    acquisition
    storage
    plugin
)

foreach(header_name IN LISTS RODAQ_ABI_HEADER_NAMES)
    add_executable(
        "rodaq_abi_c_${header_name}_header_test"
        "c/${header_name}_header_test.c"
    )
    target_link_libraries("rodaq_abi_c_${header_name}_header_test" PRIVATE rodaq_headers)
    rodaq_enable_c_warnings("rodaq_abi_c_${header_name}_header_test")
    rodaq_enable_sanitizers("rodaq_abi_c_${header_name}_header_test")
    add_test(
        NAME "rodaq.abi.c.${header_name}_header"
        COMMAND "rodaq_abi_c_${header_name}_header_test"
    )

    add_executable(
        "rodaq_abi_cpp_${header_name}_header_test"
        "cpp/${header_name}_header_test.cpp"
    )
    target_link_libraries("rodaq_abi_cpp_${header_name}_header_test" PRIVATE rodaq_headers)
    rodaq_enable_warnings("rodaq_abi_cpp_${header_name}_header_test")
    rodaq_enable_sanitizers("rodaq_abi_cpp_${header_name}_header_test")
    add_test(
        NAME "rodaq.abi.cpp.${header_name}_header"
        COMMAND "rodaq_abi_cpp_${header_name}_header_test"
    )
endforeach()

add_executable(rodaq_abi_frozen_layout_c_test layout/frozen_layout_test.c)
target_link_libraries(rodaq_abi_frozen_layout_c_test PRIVATE rodaq_headers)
rodaq_enable_c_warnings(rodaq_abi_frozen_layout_c_test)
rodaq_enable_sanitizers(rodaq_abi_frozen_layout_c_test)
add_test(NAME rodaq.abi.frozen_layout.c COMMAND rodaq_abi_frozen_layout_c_test)

add_executable(rodaq_abi_frozen_layout_cpp_test layout/frozen_layout_test.cpp)
target_link_libraries(rodaq_abi_frozen_layout_cpp_test PRIVATE rodaq_headers)
rodaq_enable_warnings(rodaq_abi_frozen_layout_cpp_test)
rodaq_enable_sanitizers(rodaq_abi_frozen_layout_cpp_test)
add_test(NAME rodaq.abi.frozen_layout.cpp COMMAND rodaq_abi_frozen_layout_cpp_test)

add_executable(rodaq_abi_minimum_prefix_c_test layout/minimum_prefix_test.c)
target_link_libraries(rodaq_abi_minimum_prefix_c_test PRIVATE rodaq_headers)
rodaq_enable_c_warnings(rodaq_abi_minimum_prefix_c_test)
rodaq_enable_sanitizers(rodaq_abi_minimum_prefix_c_test)
add_test(NAME rodaq.abi.minimum_prefix.c COMMAND rodaq_abi_minimum_prefix_c_test)

add_executable(rodaq_abi_minimum_prefix_cpp_test layout/minimum_prefix_test.cpp)
target_link_libraries(rodaq_abi_minimum_prefix_cpp_test PRIVATE rodaq_headers)
rodaq_enable_warnings(rodaq_abi_minimum_prefix_cpp_test)
rodaq_enable_sanitizers(rodaq_abi_minimum_prefix_cpp_test)
add_test(NAME rodaq.abi.minimum_prefix.cpp COMMAND rodaq_abi_minimum_prefix_cpp_test)

add_executable(rodaq_abi_scalar_c_test layout/scalar_value_test.c)
target_link_libraries(rodaq_abi_scalar_c_test PRIVATE rodaq_headers)
rodaq_enable_c_warnings(rodaq_abi_scalar_c_test)
rodaq_enable_sanitizers(rodaq_abi_scalar_c_test)
add_test(NAME rodaq.abi.scalar.c COMMAND rodaq_abi_scalar_c_test)

add_executable(rodaq_abi_scalar_cpp_test layout/scalar_value_test.cpp)
target_link_libraries(rodaq_abi_scalar_cpp_test PRIVATE rodaq_headers)
rodaq_enable_warnings(rodaq_abi_scalar_cpp_test)
rodaq_enable_sanitizers(rodaq_abi_scalar_cpp_test)
add_test(NAME rodaq.abi.scalar.cpp COMMAND rodaq_abi_scalar_cpp_test)

add_executable(rodaq_abi_tail_field_test layout/tail_field_test.cpp)
target_link_libraries(rodaq_abi_tail_field_test
    PRIVATE
        GTest::gtest_main
        rodaq_headers
)
rodaq_enable_warnings(rodaq_abi_tail_field_test)
rodaq_enable_sanitizers(rodaq_abi_tail_field_test)
add_test(NAME rodaq.abi.tail_field COMMAND rodaq_abi_tail_field_test)

add_executable(rodaq_abi_scalar_contract_test layout/scalar_contract_test.cpp)
target_link_libraries(rodaq_abi_scalar_contract_test
    PRIVATE
        GTest::gtest_main
        rodaq_headers
)
rodaq_enable_warnings(rodaq_abi_scalar_contract_test)
rodaq_enable_sanitizers(rodaq_abi_scalar_contract_test)
add_test(NAME rodaq.abi.scalar_contract COMMAND rodaq_abi_scalar_contract_test)

add_test(
    NAME rodaq.abi.header_isolation
    COMMAND
        "${CMAKE_COMMAND}"
        "-DRODAQ_TEST_ABI_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include/rodaq/plugin/abi"
        "-DRODAQ_TEST_PUBLIC_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include/rodaq"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/check_header_isolation.cmake"
)

function(rodaq_add_compile_fail_test test_name source_file fake_path diagnostic)
    set(test_object "${CMAKE_CURRENT_BINARY_DIR}/compile_fail/${test_name}.o")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/compile_fail")
    add_test(
        NAME "rodaq.compile_fail.${test_name}"
        COMMAND
            "${CMAKE_COMMAND}"
            "-DRODAQ_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DRODAQ_TEST_FAKE_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/fake_expected/${fake_path}"
            "-DRODAQ_TEST_PROJECT_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include"
            "-DRODAQ_TEST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/${source_file}"
            "-DRODAQ_TEST_OBJECT=${test_object}"
            "-DRODAQ_TEST_EXPECTED_DIAGNOSTIC=${diagnostic}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/run_compile_fail.cmake"
    )
endfunction()

rodaq_add_compile_fail_test(
    result_wrong_expected_version
    result_wrong_expected_version.cpp
    wrong_version
    "RoDAQ V1 requires expected-lite 0.10.0"
)

rodaq_add_compile_fail_test(
    result_std_expected_selection
    result_std_expected_selection.cpp
    std_selection
    "RoDAQ V1 requires expected-lite's nonstd implementation"
)

add_test(
    NAME rodaq.abi.install_smoke
    COMMAND
        "${CMAKE_COMMAND}"
        "-DRODAQ_TEST_CMAKE_COMMAND=${CMAKE_COMMAND}"
        "-DRODAQ_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}"
        "-DRODAQ_TEST_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/install-prefix"
        "-DRODAQ_TEST_OBJECT_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-objects"
        "-DRODAQ_TEST_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DRODAQ_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DRODAQ_TEST_C_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/install/c_consumer.c"
        "-DRODAQ_TEST_CPP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/install/cpp_consumer.cpp"
        "-DRODAQ_TEST_RESULT_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/install/result_consumer.cpp"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/run_install_smoke.cmake"
)

add_library(rodaq_acquisition_symbol_fixture SHARED symbol/acquisition_symbol_fixture.cpp)
target_link_libraries(rodaq_acquisition_symbol_fixture PRIVATE rodaq_headers)
set_target_properties(rodaq_acquisition_symbol_fixture PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_link_options(
    rodaq_acquisition_symbol_fixture
    PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/symbol/acquisition_symbol_fixture.map"
)
rodaq_enable_warnings(rodaq_acquisition_symbol_fixture)
rodaq_enable_sanitizers(rodaq_acquisition_symbol_fixture)

add_library(rodaq_storage_symbol_fixture SHARED symbol/storage_symbol_fixture.cpp)
target_link_libraries(rodaq_storage_symbol_fixture PRIVATE rodaq_headers)
set_target_properties(rodaq_storage_symbol_fixture PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_link_options(
    rodaq_storage_symbol_fixture
    PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/symbol/storage_symbol_fixture.map"
)
rodaq_enable_warnings(rodaq_storage_symbol_fixture)
rodaq_enable_sanitizers(rodaq_storage_symbol_fixture)

add_test(
    NAME rodaq.abi.symbol.acquisition
    COMMAND
        "${CMAKE_COMMAND}"
        "-DRODAQ_TEST_NM=${CMAKE_NM}"
        "-DRODAQ_TEST_READELF=${CMAKE_READELF}"
        "-DRODAQ_TEST_LIBRARY=$<TARGET_FILE:rodaq_acquisition_symbol_fixture>"
        "-DRODAQ_EXPECTED_SYMBOLS=rodaq_plugin_abi_version,rodaq_get_acquisition_plugin"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/check_symbols.cmake"
)

add_test(
    NAME rodaq.abi.symbol.storage
    COMMAND
        "${CMAKE_COMMAND}"
        "-DRODAQ_TEST_NM=${CMAKE_NM}"
        "-DRODAQ_TEST_READELF=${CMAKE_READELF}"
        "-DRODAQ_TEST_LIBRARY=$<TARGET_FILE:rodaq_storage_symbol_fixture>"
        "-DRODAQ_EXPECTED_SYMBOLS=rodaq_plugin_abi_version,rodaq_get_storage_plugin"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/check_symbols.cmake"
)
