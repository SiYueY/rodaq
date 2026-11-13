if(NOT DEFINED RODAQ_SOURCE_DIR)
    message(FATAL_ERROR "RODAQ_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE plugin_headers
    "${RODAQ_SOURCE_DIR}/include/rodaq/plugin/*")

foreach(header IN LISTS plugin_headers)
    get_filename_component(header_name "${header}" NAME)
    if(header_name MATCHES "^(acquisition|storage|ros2|mcap)")
        message(FATAL_ERROR "Business-specific Framework public header: ${header}")
    endif()
endforeach()

file(GLOB_RECURSE framework_sources
    "${RODAQ_SOURCE_DIR}/src/plugin/*.cpp"
    "${RODAQ_SOURCE_DIR}/src/plugin/*.hpp")

foreach(source IN LISTS framework_sources)
    file(READ "${source}" content)
    if(content MATCHES "rclcpp|rosbag2|mcap::")
        message(FATAL_ERROR "Reference Plugin dependency leaked into Framework source: ${source}")
    endif()
endforeach()

if(DEFINED RODAQ_RUNTIME_LINK_FILE AND EXISTS "${RODAQ_RUNTIME_LINK_FILE}")
    file(READ "${RODAQ_RUNTIME_LINK_FILE}" link_line)
    if(link_line MATCHES "rclcpp|rosbag2|mcap")
        message(FATAL_ERROR "Reference dependency leaked into Framework link closure")
    endif()
endif()
