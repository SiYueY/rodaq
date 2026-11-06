cmake_minimum_required(VERSION 3.22)
project(rodaq_consumer LANGUAGES CXX)

find_package(RoDAQ CONFIG REQUIRED)

add_executable(rodaq_consumer main.cpp)
target_compile_features(rodaq_consumer PRIVATE cxx_std_17)
target_link_libraries(rodaq_consumer PRIVATE rodaq::daq)
