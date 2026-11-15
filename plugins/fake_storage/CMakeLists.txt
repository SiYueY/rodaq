add_library(rodaq_fake_storage SHARED storage.cpp)
target_link_libraries(rodaq_fake_storage PRIVATE rodaq::plugin nlohmann_json::nlohmann_json Threads::Threads)
rodaq_enable_project_warnings(rodaq_fake_storage)
rodaq_register_plugin(rodaq_fake_storage
    PLUGIN_ID "rodaq.fake.storage" PLUGIN_NAME "RoDAQ Behavioral Fake Storage" PLUGIN_VERSION "1.0.0")
