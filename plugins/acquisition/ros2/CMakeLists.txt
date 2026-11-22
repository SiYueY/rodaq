find_package(rclcpp REQUIRED)

add_library(rodaq_ros2_config STATIC config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/persistence/strict_json.cpp)
target_link_libraries(rodaq_ros2_config PUBLIC rodaq_ros2_support
    PRIVATE rclcpp::rclcpp nlohmann_json::nlohmann_json)
target_include_directories(rodaq_ros2_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
rodaq_enable_project_warnings(rodaq_ros2_config)

add_library(rodaq_ros2_runtime STATIC runtime.cpp subscription.cpp native_resources.cpp)
target_link_libraries(rodaq_ros2_runtime PUBLIC rodaq_ros2_config rclcpp::rclcpp)
rodaq_enable_project_warnings(rodaq_ros2_runtime)

add_library(rodaq_ros2_acquisition SHARED plugin.cpp)
target_link_libraries(rodaq_ros2_acquisition PRIVATE rodaq_ros2_runtime)
rodaq_enable_project_warnings(rodaq_ros2_acquisition)
rodaq_register_plugin(rodaq_ros2_acquisition
    PLUGIN_ID "com.rodaq.acquisition.ros2" PLUGIN_NAME "RoDAQ ROS 2 Acquisition" PLUGIN_VERSION "1.0.0")
