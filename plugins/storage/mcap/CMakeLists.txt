find_package(mcap_vendor REQUIRED)
set(RODAQ_MCAP_ROS_DISTRO "$ENV{ROS_DISTRO}" CACHE STRING "Qualified MCAP recorder ROS distro")
if(NOT RODAQ_MCAP_ROS_DISTRO AND mcap_vendor_DIR MATCHES "/(humble|jazzy)/")
    set(RODAQ_MCAP_ROS_DISTRO "${CMAKE_MATCH_1}")
endif()
if(NOT RODAQ_MCAP_ROS_DISTRO MATCHES "^(humble|jazzy)$")
    message(FATAL_ERROR "Set RODAQ_MCAP_ROS_DISTRO to the prepared Humble or Jazzy dependency profile")
endif()

add_library(rodaq_mcap_format STATIC checked_sink.cpp rodaqmeta.cpp qos.cpp config.cpp storage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/persistence/strict_json.cpp)
target_compile_definitions(rodaq_mcap_format PRIVATE RODAQ_MCAP_ROS_DISTRO="${RODAQ_MCAP_ROS_DISTRO}")
target_link_libraries(rodaq_mcap_format PUBLIC rodaq::plugin mcap_vendor::mcap
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(rodaq_mcap_format PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
rodaq_enable_project_warnings(rodaq_mcap_format)

add_library(rodaq_mcap_storage SHARED plugin.cpp)
target_link_libraries(rodaq_mcap_storage PRIVATE rodaq_mcap_format)
rodaq_enable_project_warnings(rodaq_mcap_storage)
rodaq_register_plugin(rodaq_mcap_storage
    PLUGIN_ID "com.rodaq.storage.mcap" PLUGIN_NAME "RoDAQ MCAP Storage" PLUGIN_VERSION "1.0.0")
