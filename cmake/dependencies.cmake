include_guard(GLOBAL)

set(RODAQ_THIRD_PARTY_ROOT "${PROJECT_SOURCE_DIR}/third_party")

# Git submodule gitlinks are the authoritative commit pins. Validate only the
# groups that participate in the requested build.
function(rodaq_require_dependency name path)
    if(NOT IS_DIRECTORY "${PROJECT_SOURCE_DIR}/${path}")
        message(FATAL_ERROR
            "Missing pinned dependency '${name}'. "
            "Run: git submodule update --init --recursive"
        )
    endif()
endfunction()

foreach(required_dependency
    "expected-lite|third_party/expected-lite"
    "spdlog|third_party/spdlog"
    "fmt|third_party/fmt"
    "hdr_histogram|third_party/hdr_histogram"
    "yaml-cpp|third_party/yaml-cpp"
)
    string(REPLACE "|" ";" fields "${required_dependency}")
    list(GET fields 0 dependency_name)
    list(GET fields 1 dependency_path)
    rodaq_require_dependency("${dependency_name}" "${dependency_path}")
endforeach()

set(RODAQ_EXPECTED_LITE_INCLUDE_DIR
    "${RODAQ_THIRD_PARTY_ROOT}/expected-lite/include"
)

if(NOT EXISTS "${RODAQ_EXPECTED_LITE_INCLUDE_DIR}/nonstd/expected.hpp")
    message(FATAL_ERROR
        "The pinned expected-lite header is unavailable. "
        "Run: git submodule update --init --recursive"
    )
endif()
add_library(rodaq_expected_lite INTERFACE)
target_compile_features(rodaq_expected_lite INTERFACE cxx_std_17)
target_include_directories(rodaq_expected_lite
    SYSTEM
        INTERFACE
            "$<BUILD_INTERFACE:${RODAQ_EXPECTED_LITE_INCLUDE_DIR}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_definitions(rodaq_expected_lite
    INTERFACE
        nsel_CONFIG_SELECT_EXPECTED=nsel_EXPECTED_NONSTD
)

set(HDR_HISTOGRAM_CORE_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory(
    "${RODAQ_THIRD_PARTY_ROOT}/hdr_histogram"
    "${CMAKE_BINARY_DIR}/third_party/hdr_histogram"
)

set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(
    "${RODAQ_THIRD_PARTY_ROOT}/fmt"
    "${CMAKE_BINARY_DIR}/third_party/fmt"
    EXCLUDE_FROM_ALL
)

set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(
    "${RODAQ_THIRD_PARTY_ROOT}/spdlog"
    "${CMAKE_BINARY_DIR}/third_party/spdlog"
    EXCLUDE_FROM_ALL
)

set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
add_subdirectory(
    "${RODAQ_THIRD_PARTY_ROOT}/yaml-cpp"
    "${CMAKE_BINARY_DIR}/third_party/yaml-cpp"
    EXCLUDE_FROM_ALL
)
target_include_directories(yaml-cpp SYSTEM
    INTERFACE
        "$<BUILD_INTERFACE:${RODAQ_THIRD_PARTY_ROOT}/yaml-cpp/include>"
)

if(BUILD_TESTING AND RODAQ_BUILD_TESTS)
    rodaq_require_dependency("googletest" "third_party/googletest")
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${RODAQ_THIRD_PARTY_ROOT}/googletest"
        "${CMAKE_BINARY_DIR}/third_party/googletest"
    )
endif()

if(RODAQ_BUILD_BENCHMARKS)
    rodaq_require_dependency("benchmark" "third_party/benchmark")
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${RODAQ_THIRD_PARTY_ROOT}/benchmark"
        "${CMAKE_BINARY_DIR}/third_party/benchmark"
    )
endif()
