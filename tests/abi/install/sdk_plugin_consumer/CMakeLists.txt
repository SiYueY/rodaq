cmake_minimum_required(VERSION 3.22)
project(RoDAQSdkPluginConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(RoDAQ CONFIG REQUIRED)

add_library(consumer_sdk_plugin SHARED plugin.cpp)
rodaq_configure_plugin(
    TARGET consumer_sdk_plugin
    FAMILY acquisition
    MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
)
