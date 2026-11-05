set(RODAQ_RUNTIME_SOURCES
    active_runtime_state.cpp
    admission.cpp
    clock_correlation.cpp
    failure_signal.cpp
    interop_projection_registry.cpp
    latency_histogram.cpp
    logging_context.cpp
    logger.cpp
    payload.cpp
    payload_pool.cpp
    quarantine.cpp
    resource_budget.cpp
    runtime.cpp
    runtime_record.cpp
    runtime_stats.cpp
    storage_pipeline.cpp
    structural_arena.cpp
    spdlog_backend.cpp
)

add_library(rodaq_runtime_objects OBJECT ${RODAQ_RUNTIME_SOURCES})
target_compile_features(rodaq_runtime_objects PUBLIC cxx_std_17)
target_include_directories(rodaq_runtime_objects
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(rodaq_runtime_objects
    PUBLIC
        rodaq_foundation
    PRIVATE
        rodaq_plugin
        fmt::fmt
        hdr_histogram::hdr_histogram_core_static
        spdlog::spdlog
)
set_target_properties(rodaq_runtime_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_definitions(rodaq_runtime_objects PRIVATE RODAQ_RUNTIME_EXPORTS)
rodaq_enable_warnings(rodaq_runtime_objects)
rodaq_enable_sanitizers(rodaq_runtime_objects)

add_library(rodaq_runtime SHARED $<TARGET_OBJECTS:rodaq_runtime_objects>)
add_library(rodaq::runtime ALIAS rodaq_runtime)
set_target_properties(rodaq_runtime PROPERTIES EXPORT_NAME runtime)
target_compile_features(rodaq_runtime PUBLIC cxx_std_17)
target_include_directories(rodaq_runtime
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(rodaq_runtime
    PUBLIC
        rodaq_foundation
    PRIVATE
        rodaq_plugin
        fmt::fmt
        hdr_histogram::hdr_histogram_core_static
        spdlog::spdlog
)
set_target_properties(rodaq_runtime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
if(UNIX AND NOT APPLE)
    target_link_options(rodaq_runtime PRIVATE "LINKER:--exclude-libs,ALL")
endif()

rodaq_enable_warnings(rodaq_runtime)
rodaq_enable_sanitizers(rodaq_runtime)

if((BUILD_TESTING AND RODAQ_BUILD_TESTS) OR RODAQ_BUILD_BENCHMARKS)
    # White-box tests need private primitives that are intentionally hidden
    # from the shipped Runtime DSO. Reuse the production object files so this
    # archive cannot drift from the shared Runtime implementation.
    add_library(rodaq_runtime_testing STATIC $<TARGET_OBJECTS:rodaq_runtime_objects>)
    target_compile_features(rodaq_runtime_testing PUBLIC cxx_std_17)
    target_include_directories(rodaq_runtime_testing
        PUBLIC
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(rodaq_runtime_testing
        PUBLIC
            rodaq_plugin
        PRIVATE
            rodaq_foundation
            fmt::fmt
            hdr_histogram::hdr_histogram_core_static
            spdlog::spdlog
    )
    set_target_properties(rodaq_runtime_testing PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
    rodaq_enable_warnings(rodaq_runtime_testing)
    rodaq_enable_sanitizers(rodaq_runtime_testing)
endif()
