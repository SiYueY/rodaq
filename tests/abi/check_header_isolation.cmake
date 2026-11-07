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

set(canonical_header "${RODAQ_TEST_ABI_INCLUDE_DIR}/canonical.h")
set(storage_header "${RODAQ_TEST_ABI_INCLUDE_DIR}/storage.h")
file(READ "${canonical_header}" canonical_contents)
file(READ "${storage_header}" storage_contents)

string(FIND "${canonical_contents}" "typedef struct rodaq_storage_operation" canonical_operation)
if(NOT canonical_operation EQUAL -1)
    message(FATAL_ERROR "rodaq_storage_operation must not be defined in canonical.h")
endif()

string(FIND "${storage_contents}" "typedef struct rodaq_storage_operation" storage_operation)
if(storage_operation EQUAL -1)
    message(FATAL_ERROR "rodaq_storage_operation must be defined in storage.h")
endif()

file(GLOB public_cpp_headers "${RODAQ_TEST_PUBLIC_INCLUDE_DIR}/*.hpp")
set(expected_exposures)

foreach(header IN LISTS public_cpp_headers)
    file(READ "${header}" contents)
    string(REGEX MATCHALL "tl::[A-Za-z_][A-Za-z0-9_]*" matches "${contents}")
    get_filename_component(header_name "${header}" NAME)
    if(matches AND NOT header_name STREQUAL "result.hpp")
        message(FATAL_ERROR "${header_name} must not expose tl::expected names: ${matches}")
    endif()
    list(APPEND expected_exposures ${matches})
endforeach()

list(LENGTH expected_exposures expected_exposure_count)
if(expected_exposure_count EQUAL 0)
    message(FATAL_ERROR
        "Result header must use tl::expected as its private implementation")
endif()
