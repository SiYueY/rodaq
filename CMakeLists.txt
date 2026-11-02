cmake_minimum_required(VERSION 3.22)

project(
    RoDAQ
    VERSION 0.1.0
    DESCRIPTION "Robot data acquisition framework"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(options)
include(warnings)
include(sanitizers)
include(dependencies)

add_subdirectory(src)

if(BUILD_TESTING AND RODAQ_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(RODAQ_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

include(install)
