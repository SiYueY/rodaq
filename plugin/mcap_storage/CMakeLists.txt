add_library(rodaq_mcap_reference_plugin SHARED src/plugin.cpp)
target_include_directories(
    rodaq_mcap_reference_plugin
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(
    rodaq_mcap_reference_plugin
    PRIVATE
        mcap_vendor::mcap
        nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_mcap_reference_plugin)

rodaq_register_plugin(
    rodaq_mcap_reference_plugin
    PLUGIN_ID com.rodaq.reference.mcap
    PLUGIN_NAME "MCAP Reference Storage"
    PLUGIN_VERSION "1.0.0")
