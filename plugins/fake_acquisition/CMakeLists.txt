add_library(rodaq_fake_acquisition SHARED acquisition.cpp)
target_link_libraries(rodaq_fake_acquisition PRIVATE rodaq::plugin nlohmann_json::nlohmann_json Threads::Threads)
rodaq_enable_project_warnings(rodaq_fake_acquisition)
rodaq_register_plugin(rodaq_fake_acquisition
    PLUGIN_ID "rodaq.fake.acquisition" PLUGIN_NAME "RoDAQ Behavioral Fake Acquisition" PLUGIN_VERSION "1.0.0")
