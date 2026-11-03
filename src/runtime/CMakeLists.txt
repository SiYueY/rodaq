add_library(rodaq_runtime STATIC
    admission.cpp
    clock_correlation.cpp
    interop_projection_registry.cpp
    latency_histogram.cpp
    logger.cpp
    payload.cpp
    payload_pool.cpp
    resource_budget.cpp
    runtime_record.cpp
    runtime_stats.cpp
    structural_arena.cpp
)

target_compile_features(rodaq_runtime PUBLIC cxx_std_17)
target_include_directories(rodaq_runtime PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(rodaq_runtime
    PUBLIC
        rodaq_foundation
    PRIVATE
        hdr_histogram::hdr_histogram_core_static
)
set_target_properties(rodaq_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

rodaq_enable_warnings(rodaq_runtime)
rodaq_enable_sanitizers(rodaq_runtime)
