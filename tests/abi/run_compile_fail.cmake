execute_process(
    COMMAND
        "${RODAQ_TEST_CXX_COMPILER}"
        -std=c++17
        -Werror=unused-result
        "-I${RODAQ_TEST_FAKE_INCLUDE_DIR}"
        "-I${RODAQ_TEST_PROJECT_INCLUDE_DIR}"
        -c
        "${RODAQ_TEST_SOURCE}"
        -o
        "${RODAQ_TEST_OBJECT}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
)

if(compile_result EQUAL 0)
    message(FATAL_ERROR
        "Compilation unexpectedly succeeded for ${RODAQ_TEST_SOURCE}")
endif()

set(compile_output "${compile_stdout}\n${compile_stderr}")
string(FIND "${compile_output}" "${RODAQ_TEST_EXPECTED_DIAGNOSTIC}" diagnostic_position)

if(diagnostic_position EQUAL -1)
    message(FATAL_ERROR
        "Compilation failed for an unexpected reason.\n"
        "Expected diagnostic: ${RODAQ_TEST_EXPECTED_DIAGNOSTIC}\n"
        "Actual output:\n${compile_output}")
endif()
