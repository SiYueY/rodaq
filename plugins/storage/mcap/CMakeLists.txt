find_package(mcap_vendor REQUIRED)

add_library(rodaq_mcap_format STATIC checked_sink.cpp rodaqmeta.cpp)
target_link_libraries(rodaq_mcap_format PUBLIC rodaq::plugin mcap_vendor::mcap)
target_include_directories(rodaq_mcap_format PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
rodaq_enable_project_warnings(rodaq_mcap_format)
