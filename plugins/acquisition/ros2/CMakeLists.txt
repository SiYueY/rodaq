find_package(rclcpp REQUIRED)

add_library(rodaq_ros2_config STATIC config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/persistence/strict_json.cpp)
target_link_libraries(rodaq_ros2_config PUBLIC rodaq_ros2_support
    PRIVATE rclcpp::rclcpp nlohmann_json::nlohmann_json)
target_include_directories(rodaq_ros2_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
rodaq_enable_project_warnings(rodaq_ros2_config)
