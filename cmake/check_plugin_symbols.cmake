execute_process(
    COMMAND "${RODAQ_TEST_NM}" -D --defined-only "${RODAQ_TEST_LIBRARY}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE nm_output
    ERROR_VARIABLE nm_error
)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "Plugin symbol check failed: ${nm_error}")
endif()

string(REPLACE "\r\n" "\n" nm_output "${nm_output}")
string(REPLACE "\n" ";" nm_lines "${nm_output}")
set(actual_symbols)
foreach(line IN LISTS nm_lines)
    if(line MATCHES "^[0-9A-Fa-f]+[ \t]+[A-Za-z][ \t]+([^ \t]+)$")
        list(APPEND actual_symbols "${CMAKE_MATCH_1}")
    endif()
endforeach()
string(REPLACE "," ";" expected_symbols "${RODAQ_EXPECTED_SYMBOLS}")
list(SORT actual_symbols)
list(SORT expected_symbols)
if(NOT actual_symbols STREQUAL expected_symbols)
    message(FATAL_ERROR
        "Plugin exported symbol mismatch.\nExpected: ${expected_symbols}\nActual: ${actual_symbols}")
endif()
