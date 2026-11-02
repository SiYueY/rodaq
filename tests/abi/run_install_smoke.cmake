file(REMOVE_RECURSE "${RODAQ_TEST_INSTALL_PREFIX}")
file(MAKE_DIRECTORY "${RODAQ_TEST_OBJECT_DIR}")

execute_process(
    COMMAND
        "${RODAQ_TEST_CMAKE_COMMAND}"
        --install
        "${RODAQ_TEST_BINARY_DIR}"
        --prefix
        "${RODAQ_TEST_INSTALL_PREFIX}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)

if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "RoDAQ install failed.\n${install_stdout}\n${install_stderr}")
endif()

set(include_dir "${RODAQ_TEST_INSTALL_PREFIX}/include")
set(plugin_abi_dir "${include_dir}/rodaq/plugin/abi")
set(legacy_abi_dir "${include_dir}/rodaq/abi")
set(expected_license
    "${RODAQ_TEST_INSTALL_PREFIX}/share/rodaq/licenses/expected-lite/LICENSE.txt")

foreach(header_name common canonical service config runtime acquisition storage plugin)
    if(NOT EXISTS "${plugin_abi_dir}/${header_name}.h")
        message(FATAL_ERROR
            "Installed Plugin ABI header is missing: ${header_name}.h")
    endif()
endforeach()

foreach(sdk_header
        abi_support.hpp
        config_view.hpp
        service_registry.hpp
        runtime_services.hpp
        plugin.hpp
        acquisition_plugin.hpp
        storage_plugin.hpp)
    if(NOT EXISTS "${include_dir}/rodaq/plugin/${sdk_header}")
        message(FATAL_ERROR "Installed SDK header is missing: ${sdk_header}")
    endif()
endforeach()

if(EXISTS "${legacy_abi_dir}")
    message(FATAL_ERROR "Legacy installed ABI directory still exists: ${legacy_abi_dir}")
endif()

if(NOT EXISTS "${expected_license}")
    message(FATAL_ERROR "Installed expected-lite license is missing")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_C_COMPILER}"
        -std=c11
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        "-I${include_dir}"
        -c
        "${RODAQ_TEST_C_SOURCE}"
        -o
        "${RODAQ_TEST_OBJECT_DIR}/c_consumer.o"
    RESULT_VARIABLE c_result
    OUTPUT_VARIABLE c_stdout
    ERROR_VARIABLE c_stderr
)

if(NOT c_result EQUAL 0)
    message(FATAL_ERROR
        "Installed C ABI consumer failed to compile.\n${c_stdout}\n${c_stderr}")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_CXX_COMPILER}"
        -std=c++17
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        "-I${include_dir}"
        -c
        "${RODAQ_TEST_CPP_SOURCE}"
        -o
        "${RODAQ_TEST_OBJECT_DIR}/cpp_consumer.o"
    RESULT_VARIABLE cpp_result
    OUTPUT_VARIABLE cpp_stdout
    ERROR_VARIABLE cpp_stderr
)

if(NOT cpp_result EQUAL 0)
    message(FATAL_ERROR
        "Installed C++ ABI consumer failed to compile.\n${cpp_stdout}\n${cpp_stderr}")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_CXX_COMPILER}"
        -std=c++17
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        "-I${include_dir}"
        -c
        "${RODAQ_TEST_RESULT_SOURCE}"
        -o
        "${RODAQ_TEST_OBJECT_DIR}/result_consumer.o"
    RESULT_VARIABLE result_result
    OUTPUT_VARIABLE result_stdout
    ERROR_VARIABLE result_stderr
)

if(NOT result_result EQUAL 0)
    message(FATAL_ERROR
        "Installed Result consumer failed to compile.\n"
        "${result_stdout}\n${result_stderr}")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_CMAKE_COMMAND}"
        -S
        "${RODAQ_TEST_SDK_PLUGIN_SOURCE_DIR}"
        -B
        "${RODAQ_TEST_OBJECT_DIR}/sdk-plugin-consumer"
        "-DCMAKE_PREFIX_PATH=${RODAQ_TEST_INSTALL_PREFIX}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Installed SDK Plugin consumer failed to configure.\n"
        "${configure_stdout}\n${configure_stderr}")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_CMAKE_COMMAND}"
        --build
        "${RODAQ_TEST_OBJECT_DIR}/sdk-plugin-consumer"
    RESULT_VARIABLE sdk_build_result
    OUTPUT_VARIABLE sdk_build_stdout
    ERROR_VARIABLE sdk_build_stderr
)
if(NOT sdk_build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed SDK Plugin consumer failed to build.\n"
        "${sdk_build_stdout}\n${sdk_build_stderr}")
endif()
