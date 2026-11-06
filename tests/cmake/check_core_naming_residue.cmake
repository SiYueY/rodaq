if(NOT DEFINED RODAQ_SOURCE_ROOT)
    message(FATAL_ERROR "RODAQ_SOURCE_ROOT is required")
endif()

foreach(forbidden_path
        "src/runtime"
        "src/sdk"
        "src/detail"
        "src/internal"
        "src/common"
        "src/core"
        "src/failure"
        "src/resource"
        "src/observability"
        "include/rodaq/runtime"
        "tests/runtime"
        "tests/observability")
    if(EXISTS "${RODAQ_SOURCE_ROOT}/${forbidden_path}")
        message(FATAL_ERROR "Forbidden source path exists: ${forbidden_path}")
    endif()
endforeach()

file(GLOB_RECURSE residue_files
    "${RODAQ_SOURCE_ROOT}/src/*.cpp"
    "${RODAQ_SOURCE_ROOT}/src/*.hpp"
    "${RODAQ_SOURCE_ROOT}/include/*.h"
    "${RODAQ_SOURCE_ROOT}/include/*.hpp"
    "${RODAQ_SOURCE_ROOT}/tests/*.cpp"
    "${RODAQ_SOURCE_ROOT}/tests/*.h"
    "${RODAQ_SOURCE_ROOT}/tests/*.hpp"
    "${RODAQ_SOURCE_ROOT}/benchmarks/*.cpp"
    "${RODAQ_SOURCE_ROOT}/benchmarks/*.hpp"
)

set(forbidden_patterns
    "InterfaceView"
    "ServiceRegistryView"
    "ConfigNodeView"
    "rodaq_(source|clock|schema|stream|stream_descriptor|timestamp_key_set|projection)_view"
    "_VIEW_MIN_STRUCT_SIZE"
    "rodaq::(runtime|detail|internal|impl)"
    "namespace[ \t\r\n]+(rodaq::)?(runtime|detail|internal|impl)[ \t\r\n{]"
    "(^|[^A-Za-z0-9_])(Runtime|RuntimeConfig|RuntimeState|ActiveRuntimeState)([^A-Za-z0-9_]|$)"
    "_v1"
    "[.]v1"
    "V1 minimum"
    "V1 requires"
)

foreach(source_file IN LISTS residue_files)
    file(READ "${source_file}" source_text)
    foreach(forbidden_pattern IN LISTS forbidden_patterns)
        if(source_text MATCHES "${forbidden_pattern}")
            message(FATAL_ERROR
                "Forbidden naming residue '${forbidden_pattern}' in ${source_file}")
        endif()
    endforeach()
endforeach()
