execute_process(
    COMMAND "${RODAQ_TEST_NM}" -D --defined-only "${RODAQ_TEST_LIBRARY}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE nm_output
    ERROR_VARIABLE nm_error
)

if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "nm failed: ${nm_error}")
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
        "Exported symbol mismatch.\n"
        "Expected: ${expected_symbols}\n"
        "Actual: ${actual_symbols}")
endif()

if(RODAQ_TEST_READELF)
    execute_process(
        COMMAND "${RODAQ_TEST_READELF}" --dyn-syms --wide "${RODAQ_TEST_LIBRARY}"
        RESULT_VARIABLE readelf_result
        OUTPUT_VARIABLE readelf_output
        ERROR_VARIABLE readelf_error
    )

    if(NOT readelf_result EQUAL 0)
        message(FATAL_ERROR "readelf failed: ${readelf_error}")
    endif()

    string(REPLACE "\r\n" "\n" readelf_output "${readelf_output}")
    string(REPLACE "\n" ";" readelf_lines "${readelf_output}")
    set(readelf_symbols)

    foreach(line IN LISTS readelf_lines)
        if(line MATCHES
           "^[ ]*[0-9]+: [0-9A-Fa-f]+ +[0-9]+ +[A-Z]+ +[A-Z]+ +[A-Z]+ +([^ ]+) +(.+)$")
            if(NOT CMAKE_MATCH_1 STREQUAL "UND")
                list(APPEND readelf_symbols "${CMAKE_MATCH_2}")
            endif()
        endif()
    endforeach()

    list(SORT readelf_symbols)
    if(NOT readelf_symbols STREQUAL expected_symbols)
        message(FATAL_ERROR
            "readelf exported symbol mismatch.\n"
            "Expected: ${expected_symbols}\n"
            "Actual: ${readelf_symbols}")
    endif()
endif()
