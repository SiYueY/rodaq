add_library(rodaq_plugin STATIC
    plugin_error.cpp
    acquisition_plugin_instance.cpp
    plugin_catalog.cpp
    plugin_instance.cpp
    plugin_library.cpp
    plugin_manager.cpp
    plugin_validation.cpp
    shared_library.cpp
    storage_plugin_instance.cpp
)

target_compile_features(rodaq_plugin PUBLIC cxx_std_17)
target_include_directories(rodaq_plugin PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(rodaq_plugin PUBLIC rodaq_foundation ${CMAKE_DL_LIBS})
target_compile_definitions(rodaq_plugin PRIVATE _GNU_SOURCE)
set_target_properties(rodaq_plugin PROPERTIES POSITION_INDEPENDENT_CODE ON)

rodaq_enable_warnings(rodaq_plugin)
rodaq_enable_sanitizers(rodaq_plugin)
