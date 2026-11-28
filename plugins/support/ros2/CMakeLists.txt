find_package(ament_index_cpp REQUIRED)

# Deterministic shared ROS 2 canonicalization support contract (schema + QoS).
# Neither links rclcpp nor creates a ROS Context; exported as rodaq::ros2_support
# so official ROS 2 and future direct-DDS plugins build truly out-of-tree against
# an installed RoDAQ SDK prefix.
add_library(rodaq_ros2_support STATIC schema.cpp qos.cpp)
add_library(rodaq::ros2_support ALIAS rodaq_ros2_support)
set_target_properties(rodaq_ros2_support PROPERTIES EXPORT_NAME ros2_support)
target_compile_features(rodaq_ros2_support PUBLIC cxx_std_17)
target_compile_options(rodaq_ros2_support PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rodaq_ros2_support PUBLIC rodaq::plugin ament_index_cpp::ament_index_cpp)
target_include_directories(rodaq_ros2_support PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/rodaq/support>)

install(TARGETS rodaq_ros2_support EXPORT RoDAQTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES schema.hpp qos.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rodaq/support/ros2)
