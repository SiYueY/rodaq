include_guard(GLOBAL)

set(RODAQ_THIRD_PARTY_ROOT "${PROJECT_SOURCE_DIR}/third_party")

# Git submodule gitlinks are the authoritative commit pins. This registry only
# records human-readable release metadata and build integration paths.
# name|release|tag|path
set(RODAQ_DEPENDENCY_REGISTRY
    "expected-lite|0.10.0|v0.10.0|third_party/expected-lite"
    "onetbb|2022.3.0|v2022.3.0|third_party/onetbb"
    "spdlog|1.17.0|v1.17.0|third_party/spdlog"
    "fmt|12.1.0|12.1.0|third_party/fmt"
    "hdr_histogram|0.12.1|0.12.1|third_party/hdr_histogram"
    "yaml-cpp|0.9.0|yaml-cpp-0.9.0|third_party/yaml-cpp"
    "mimalloc|3.6.1|v3.6.1|third_party/mimalloc"
    "googletest|1.18.0|v1.18.0|third_party/googletest"
    "benchmark|1.10.0|v1.10.0|third_party/benchmark"
)

foreach(dependency IN LISTS RODAQ_DEPENDENCY_REGISTRY)
    string(REPLACE "|" ";" fields "${dependency}")
    list(GET fields 0 dependency_name)
    list(GET fields 3 dependency_path)

    if(NOT IS_DIRECTORY "${PROJECT_SOURCE_DIR}/${dependency_path}")
        message(FATAL_ERROR
            "Missing pinned dependency '${dependency_name}'. "
            "Run: git submodule update --init --recursive"
        )
    endif()
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

if(BUILD_TESTING AND RODAQ_BUILD_TESTS)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${RODAQ_THIRD_PARTY_ROOT}/googletest"
        "${CMAKE_BINARY_DIR}/third_party/googletest"
    )
endif()

if(RODAQ_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${RODAQ_THIRD_PARTY_ROOT}/benchmark"
        "${CMAKE_BINARY_DIR}/third_party/benchmark"
    )
endif()

if(RODAQ_USE_MIMALLOC)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${RODAQ_THIRD_PARTY_ROOT}/mimalloc"
        "${CMAKE_BINARY_DIR}/third_party/mimalloc"
    )
endif()
