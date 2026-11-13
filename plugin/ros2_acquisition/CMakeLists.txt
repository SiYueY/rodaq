add_library(rodaq_ros2_reference_plugin SHARED src/plugin.cpp)
target_include_directories(
    rodaq_ros2_reference_plugin
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(
    rodaq_ros2_reference_plugin
    PRIVATE
        rclcpp::rclcpp
        rosbag2_transport::rosbag2_transport
        nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_ros2_reference_plugin)

rodaq_register_plugin(
    rodaq_ros2_reference_plugin
    PLUGIN_ID com.rodaq.reference.ros2
    PLUGIN_NAME "ROS 2 Reference Acquisition"
    PLUGIN_VERSION "1.0.0")
