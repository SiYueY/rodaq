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
set(expected_license
    "${RODAQ_TEST_INSTALL_PREFIX}/share/rodaq/licenses/expected-lite/LICENSE.txt")

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
