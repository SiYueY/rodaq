add_library(rodaq_headers INTERFACE)
target_include_directories(rodaq_headers
    INTERFACE
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

add_library(rodaq_public_api INTERFACE)
target_compile_features(rodaq_public_api INTERFACE cxx_std_17)
target_link_libraries(rodaq_public_api
    INTERFACE
        rodaq_headers
        rodaq_tl_expected
        fmt::fmt
        spdlog::spdlog
)

add_library(rodaq_base_objects OBJECT
    plugin/abi_support.cpp
)
target_compile_features(rodaq_base_objects PUBLIC cxx_std_17)
target_include_directories(rodaq_base_objects
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(rodaq_base_objects PUBLIC rodaq_tl_expected)
set_target_properties(rodaq_base_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
rodaq_enable_warnings(rodaq_base_objects)
rodaq_enable_sanitizers(rodaq_base_objects)

add_library(rodaq_base STATIC $<TARGET_OBJECTS:rodaq_base_objects>)
target_compile_features(rodaq_base PUBLIC cxx_std_17)
target_include_directories(rodaq_base
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(rodaq_base PUBLIC rodaq_tl_expected)
set_target_properties(rodaq_base PROPERTIES POSITION_INDEPENDENT_CODE ON)
rodaq_enable_warnings(rodaq_base)
rodaq_enable_sanitizers(rodaq_base)

add_library(rodaq_sdk STATIC
    $<TARGET_OBJECTS:rodaq_base_objects>
    config/config.cpp
    plugin/config_view.cpp
    plugin/plugin.cpp
    plugin/services.cpp
    plugin/acquisition_validation.cpp
    plugin/service_registry_ref.cpp
    plugin/storage_validation.cpp
)
add_library(rodaq::sdk ALIAS rodaq_sdk)
target_compile_features(rodaq_sdk PUBLIC cxx_std_17)
target_include_directories(rodaq_sdk
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${RODAQ_TL_EXPECTED_INCLUDE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
set_target_properties(rodaq_sdk PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME sdk
)
rodaq_enable_warnings(rodaq_sdk)
rodaq_enable_sanitizers(rodaq_sdk)

add_subdirectory(plugin)
add_subdirectory(daq)
