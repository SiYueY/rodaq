cmake_minimum_required(VERSION 3.22)
project(rodaq_mcap_storage VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT TARGET rodaq::plugin)
    find_package(RoDAQ REQUIRED CONFIG)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json REQUIRED CONFIG)
endif()

set(RODAQ_MCAP_CPP_INCLUDE_DIR "" CACHE PATH
    "Path to standalone upstream Foxglove mcap C++ include directory (containing mcap/writer.hpp)")
if(NOT RODAQ_MCAP_CPP_INCLUDE_DIR)
    set(_default_mcap_inc "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/mcap-cpp/cpp/mcap/include")
    if(EXISTS "${_default_mcap_inc}/mcap/writer.hpp")
        get_filename_component(RODAQ_MCAP_CPP_INCLUDE_DIR "${_default_mcap_inc}" ABSOLUTE)
    else()
        message(FATAL_ERROR "Initialize the pinned MCAP submodule or supply its explicit include directory")
    endif()
endif()
if(NOT EXISTS "${RODAQ_MCAP_CPP_INCLUDE_DIR}/mcap/writer.hpp")
    message(FATAL_ERROR "Standalone upstream Foxglove mcap headers not found at: ${RODAQ_MCAP_CPP_INCLUDE_DIR}")
endif()

add_library(rodaq_mcap_format STATIC mcap_impl.cpp checked_sink.cpp qos.cpp config.cpp storage.cpp)
set_source_files_properties(mcap_impl.cpp PROPERTIES CXX_CLANG_TIDY "")
target_compile_definitions(rodaq_mcap_format PUBLIC MCAP_COMPRESSION_NO_LZ4 MCAP_COMPRESSION_NO_ZSTD)
target_compile_options(rodaq_mcap_format PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rodaq_mcap_format PUBLIC rodaq::plugin
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(rodaq_mcap_format PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Shared private source helper, bundled with isolated official plugin sources.
set(RODAQ_STRICT_JSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/internal" CACHE PATH
    "Directory containing RoDAQ's private strict_json.hpp")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/strict_json.hpp")
    set(RODAQ_STRICT_JSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
if(NOT EXISTS "${RODAQ_STRICT_JSON_INCLUDE_DIR}/strict_json.hpp")
    message(FATAL_ERROR "Official plugin source requires private strict_json.hpp")
endif()
target_include_directories(rodaq_mcap_format PRIVATE "${RODAQ_STRICT_JSON_INCLUDE_DIR}")
target_include_directories(rodaq_mcap_format SYSTEM PUBLIC ${RODAQ_MCAP_CPP_INCLUDE_DIR})

add_library(rodaq_mcap_storage SHARED plugin.cpp)
target_compile_options(rodaq_mcap_storage PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rodaq_mcap_storage PRIVATE rodaq_mcap_format)
rodaq_register_plugin(rodaq_mcap_storage
    PLUGIN_ID "com.rodaq.storage.mcap" PLUGIN_NAME "RoDAQ MCAP Storage" PLUGIN_VERSION "1.0.0")
