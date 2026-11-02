add_library(rodaq_headers INTERFACE)
target_include_directories(rodaq_headers
    INTERFACE
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)

add_library(rodaq_public_api INTERFACE)
target_compile_features(rodaq_public_api INTERFACE cxx_std_17)
target_link_libraries(rodaq_public_api INTERFACE rodaq_headers rodaq_expected_lite)

add_library(rodaq_foundation STATIC
    abi_support.cpp
    error.cpp
)
target_link_libraries(rodaq_foundation PUBLIC rodaq_public_api)
set_target_properties(rodaq_foundation PROPERTIES POSITION_INDEPENDENT_CODE ON)

rodaq_enable_warnings(rodaq_foundation)
rodaq_enable_sanitizers(rodaq_foundation)

add_subdirectory(plugin)
