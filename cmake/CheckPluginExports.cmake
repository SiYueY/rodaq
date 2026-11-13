if(NOT DEFINED RODAQ_PLUGIN_LIBRARY)
    message(FATAL_ERROR "RODAQ_PLUGIN_LIBRARY is required")
endif()

string(REPLACE "\"" "" nm_command "${CMAKE_NM}")
execute_process(
    COMMAND "${nm_command}" -D --defined-only "${RODAQ_PLUGIN_LIBRARY}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE nm_output
    ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "Plugin symbol inspection failed: ${nm_error}")
endif()

string(REPLACE "\n" ";" nm_lines "${nm_output}")
foreach(line IN LISTS nm_lines)
    if(line STREQUAL "")
        continue()
    endif()
    if(NOT line MATCHES "[ \t]([^ \t]+)$")
        continue()
    endif()
    set(symbol "${CMAKE_MATCH_1}")
    if(symbol STREQUAL "rodaq_plugin_abi_version" OR
       symbol STREQUAL "rodaq_plugin_get_entry" OR
       symbol MATCHES "^rodaq_test_")
        continue()
    endif()
    message(FATAL_ERROR "Unexpected Plugin dynamic symbol: ${symbol}")
endforeach()
