find_package(ament_index_cpp REQUIRED)

# This private canonicalization module neither links rclcpp nor creates a ROS
# Context. ROS 2 and the direct-DDS candidate share the same deterministic rules.
add_library(rodaq_ros2_support STATIC schema.cpp qos.cpp)
target_compile_options(rodaq_ros2_support PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rodaq_ros2_support PUBLIC rodaq::plugin ament_index_cpp::ament_index_cpp)
target_include_directories(rodaq_ros2_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
