file(GLOB abi_headers "${RODAQ_TEST_ABI_INCLUDE_DIR}/*.h")

set(forbidden_tokens
    "std::"
    "nonstd::"
    "tbb::"
    "spdlog::"
    "fmt::"
    "YAML"
    "rclcpp"
    "dds::"
    "eprosima"
    "mcap::"
    "sqlite3"
)

set(forbidden_includes
    "#include <string>"
    "#include <vector>"
    "#include <memory>"
    "#include <functional>"
    "#include <expected>"
)

foreach(header IN LISTS abi_headers)
    file(READ "${header}" contents)
    get_filename_component(header_name "${header}" NAME)

    foreach(token IN LISTS forbidden_tokens)
        string(FIND "${contents}" "${token}" token_position)
        if(NOT token_position EQUAL -1)
            message(FATAL_ERROR
                "${header_name} contains forbidden token '${token}'")
        endif()
    endforeach()

    foreach(include_line IN LISTS forbidden_includes)
        string(FIND "${contents}" "${include_line}" include_position)
        if(NOT include_position EQUAL -1)
            message(FATAL_ERROR
                "${header_name} contains forbidden include '${include_line}'")
        endif()
    endforeach()

    string(REGEX MATCHALL "\\(\\*" callback_openings "${contents}")
    list(LENGTH callback_openings callback_count)
    string(REGEX MATCHALL "RODAQ_NOEXCEPT" noexcept_markers "${contents}")
    list(LENGTH noexcept_markers noexcept_count)

    if(callback_count GREATER 0 AND NOT callback_count EQUAL noexcept_count)
        message(FATAL_ERROR
            "${header_name} has ${callback_count} callback declarations but "
            "${noexcept_count} RODAQ_NOEXCEPT markers")
    endif()
endforeach()

file(GLOB public_cpp_headers "${RODAQ_TEST_PUBLIC_INCLUDE_DIR}/*.hpp")
set(nonstd_exposures)

foreach(header IN LISTS public_cpp_headers)
    file(READ "${header}" contents)
    string(REGEX MATCHALL "nonstd::[A-Za-z_][A-Za-z0-9_]*" matches "${contents}")
    list(APPEND nonstd_exposures ${matches})
endforeach()

list(LENGTH nonstd_exposures nonstd_exposure_count)
if(NOT nonstd_exposure_count EQUAL 1)
    message(FATAL_ERROR
        "Public C++ headers must expose exactly one nonstd alias, found: "
        "${nonstd_exposures}")
endif()

if(NOT nonstd_exposures STREQUAL "nonstd::expected")
    message(FATAL_ERROR
        "Unexpected Public C++ nonstd exposure: ${nonstd_exposures}")
endif()
