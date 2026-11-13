cmake_minimum_required(VERSION 3.22)

project(rodaq LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RODAQ_WARNINGS_AS_ERRORS "Treat project compiler warnings as errors" OFF)
option(RODAQ_BUILD_REFERENCE_PLUGINS "Build real Reference Plugins" OFF)
option(
    RODAQ_BUILD_ROS2_REFERENCE_PLUGIN
    "Build the ROS 2 Reference Plugin"
    ${RODAQ_BUILD_REFERENCE_PLUGINS})
option(
    RODAQ_BUILD_MCAP_REFERENCE_PLUGIN
    "Build the MCAP Reference Plugin"
    ${RODAQ_BUILD_REFERENCE_PLUGINS})

include(CTest)
include(cmake/RoDAQWarnings.cmake)
include(cmake/RoDAQPlugin.cmake)

add_library(rodaq_core INTERFACE)
add_library(rodaq::core ALIAS rodaq_core)
target_compile_features(rodaq_core INTERFACE cxx_std_17)
target_include_directories(
    rodaq_core
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_include_directories(
    rodaq_core
    SYSTEM INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/tl_expected/include>)

set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_FUZZ OFF CACHE BOOL "" FORCE)
set(FMT_CUDA_TEST OFF CACHE BOOL "" FORCE)
set(FMT_OS ON CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fmt EXCLUDE_FROM_ALL)

set(SPDLOG_BUILD_ALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_SYSTEM_INCLUDES ON CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS OFF CACHE BOOL "" FORCE)
set(SPDLOG_NO_THREAD_ID OFF CACHE BOOL "" FORCE)
set(SPDLOG_NO_ATOMIC_LEVELS OFF CACHE BOOL "" FORCE)
set(SPDLOG_NO_TZ_OFFSET OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/spdlog EXCLUDE_FROM_ALL)

add_library(rodaq_logging STATIC src/logging.cpp)
add_library(rodaq::logging ALIAS rodaq_logging)
target_compile_features(rodaq_logging PUBLIC cxx_std_17)
target_include_directories(
    rodaq_logging
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(
    rodaq_logging
    PUBLIC
        rodaq_core
        fmt::fmt
    PRIVATE
        spdlog::spdlog)

add_library(rodaq_plugin_authoring INTERFACE)
target_compile_features(rodaq_plugin_authoring INTERFACE cxx_std_17)
target_include_directories(
    rodaq_plugin_authoring
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(rodaq_plugin_authoring INTERFACE rodaq::core)

find_package(OpenSSL REQUIRED)

add_library(
    rodaq_plugin_runtime STATIC
    src/plugin/abi_support.cpp
    src/plugin/artifact.cpp
    src/plugin/catalog.cpp
    src/plugin/json.cpp
    src/plugin/lifecycle_executor.cpp
    src/plugin/library_state.cpp
    src/plugin/loader.cpp
    src/plugin/manager.cpp
    src/plugin/manifest.cpp
    src/plugin/semver.cpp
    src/plugin/service_registry.cpp
    src/plugin/instance.cpp)
target_link_libraries(
    rodaq_plugin_runtime
    PUBLIC
        rodaq_plugin_authoring
        rodaq::logging
    PRIVATE
        OpenSSL::Crypto
        ${CMAKE_DL_LIBS})
rodaq_enable_project_warnings(rodaq_plugin_runtime)

add_library(rodaq_plugin INTERFACE)
target_link_libraries(
    rodaq_plugin
    INTERFACE
        rodaq_plugin_authoring
        rodaq_plugin_runtime)
add_library(rodaq::plugin ALIAS rodaq_plugin)

if(RODAQ_BUILD_REFERENCE_PLUGINS OR
   RODAQ_BUILD_ROS2_REFERENCE_PLUGIN OR
   RODAQ_BUILD_MCAP_REFERENCE_PLUGIN)
    add_subdirectory(plugin)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
    add_subdirectory(tests/plugin)
endif()
