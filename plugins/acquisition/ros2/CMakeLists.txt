cmake_minimum_required(VERSION 3.22)
project(rodaq_ros2_acquisition VERSION 1.0.0 LANGUAGES C CXX)

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
find_package(rclcpp REQUIRED)

if(NOT TARGET rodaq_ros2_support)
    find_package(ament_index_cpp REQUIRED)
    add_library(rodaq_ros2_support STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../support/ros2/schema.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../support/ros2/qos.cpp)
    target_compile_options(rodaq_ros2_support PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rodaq_ros2_support PUBLIC rodaq::plugin ament_index_cpp::ament_index_cpp)
    target_include_directories(rodaq_ros2_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../support)
endif()

add_library(rodaq_ros2_config STATIC config.cpp)
target_compile_options(rodaq_ros2_config PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rodaq_ros2_config PUBLIC rodaq_ros2_support
    PRIVATE rclcpp::rclcpp nlohmann_json::nlohmann_json)
target_include_directories(rodaq_ros2_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(rodaq_ros2_runtime STATIC runtime.cpp subscription.cpp native_resources.cpp)
target_compile_options(rodaq_ros2_runtime PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rodaq_ros2_runtime PUBLIC rodaq_ros2_config rclcpp::rclcpp)

add_library(rodaq_ros2_acquisition SHARED plugin.cpp)
target_compile_options(rodaq_ros2_acquisition PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rodaq_ros2_acquisition PRIVATE rodaq_ros2_runtime)
rodaq_register_plugin(rodaq_ros2_acquisition
    PLUGIN_ID "com.rodaq.acquisition.ros2" PLUGIN_NAME "RoDAQ ROS 2 Acquisition" PLUGIN_VERSION "1.0.0")
