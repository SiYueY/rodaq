add_library(rodaq_headers INTERFACE)
target_include_directories(rodaq_headers
    INTERFACE
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

add_library(rodaq_public_api INTERFACE)
target_compile_features(rodaq_public_api INTERFACE cxx_std_17)
target_link_libraries(rodaq_public_api INTERFACE rodaq_headers rodaq_expected_lite)

add_library(rodaq_api_support STATIC
    plugin/abi_support.cpp
    status.cpp
    result.cpp
    config/config.cpp
    config/yaml_config_loader.cpp
)
target_compile_features(rodaq_api_support PUBLIC cxx_std_17)
target_include_directories(rodaq_api_support
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(rodaq_api_support
    PUBLIC
        rodaq_public_api
    PRIVATE
        yaml-cpp::yaml-cpp
)
set_target_properties(rodaq_api_support PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME api_support
)
rodaq_enable_warnings(rodaq_api_support)
rodaq_enable_sanitizers(rodaq_api_support)

add_library(rodaq_sdk STATIC
    plugin/config_view.cpp
    plugin/plugin.cpp
    plugin/services.cpp
    plugin/acquisition_validation.cpp
    plugin/service_registry_view.cpp
    plugin/storage_validation.cpp
)
add_library(rodaq::sdk ALIAS rodaq_sdk)
target_compile_features(rodaq_sdk PUBLIC cxx_std_17)
target_include_directories(rodaq_sdk
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(rodaq_sdk PUBLIC rodaq_api_support)
set_target_properties(rodaq_sdk PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME sdk
)
rodaq_enable_warnings(rodaq_sdk)
rodaq_enable_sanitizers(rodaq_sdk)

add_subdirectory(plugin)
add_subdirectory(daq)
