execute_process(
    COMMAND "${RODAQ_TEST_COMMAND}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if(result EQUAL 0)
    message(FATAL_ERROR "Expected subprocess failure, but it succeeded")
endif()
