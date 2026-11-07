set(RODAQ_SOURCES
    daq.cpp
    lifecycle.cpp
    recovery.cpp
    failure_mailbox.cpp
    ../config/config.cpp
    ../plugin/abi_support.cpp
    ../activation/activation_state.cpp
    ../activation/structural_arena.cpp
    ../admission/admission.cpp
    ../admission/admission_state.cpp
    ../admission/acceptance_sequence.cpp
    ../admission/resource_budget.cpp
    ../admission/submission.cpp
    ../binding/binding.cpp
    ../binding/binding_state.cpp
    ../canonical/canonical_state.cpp
    ../canonical/registration.cpp
    ../canonical/descriptor.cpp
    ../canonical/availability.cpp
    ../canonical/timestamp_state.cpp
    ../record/accepted_record.cpp
    ../payload/payload.cpp
    ../payload/payload_pool.cpp
    ../storage/storage_pipeline.cpp
    ../clock/clock_correlation.cpp
    ../config/yaml_config_loader.cpp
    ../config/validation.cpp
    ../projection/projection_registry.cpp
    ../logging/logging_context.cpp
    ../logging/spdlog_backend.cpp
    ../metrics/latency_histogram.cpp
    ../metrics/stats_accumulator.cpp
    ../metrics/activation_metrics.cpp
    ../plugin/common_service_adapter.cpp
    ../plugin/canonical_service_adapter.cpp
    ../plugin/acquisition_service_adapter.cpp
    ../plugin/storage_service_adapter.cpp
    ../plugin/activation_services.cpp
)

add_library(rodaq_objects OBJECT ${RODAQ_SOURCES})
target_compile_features(rodaq_objects PUBLIC cxx_std_17)
target_include_directories(rodaq_objects
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(rodaq_objects
    PUBLIC
        rodaq_public_api
    PRIVATE
        rodaq_plugin
        rodaq_sdk
        yaml-cpp::yaml-cpp
        fmt::fmt
        hdr_histogram::hdr_histogram_core_static
        spdlog::spdlog
)
set_target_properties(rodaq_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_definitions(rodaq_objects PRIVATE RODAQ_EXPORTS)
rodaq_enable_warnings(rodaq_objects)
rodaq_enable_sanitizers(rodaq_objects)

add_library(rodaq SHARED $<TARGET_OBJECTS:rodaq_objects>)
add_library(rodaq::daq ALIAS rodaq)
set_target_properties(rodaq PROPERTIES
    EXPORT_NAME daq
    OUTPUT_NAME rodaq
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(rodaq PUBLIC cxx_std_17)
target_include_directories(rodaq
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(rodaq
    PUBLIC
        fmt::fmt
        spdlog::spdlog
    PRIVATE
        rodaq_plugin
        rodaq_sdk
        yaml-cpp::yaml-cpp
        hdr_histogram::hdr_histogram_core_static
)
if(UNIX AND NOT APPLE)
    set_property(TARGET rodaq APPEND PROPERTY
        LINK_DEPENDS "${PROJECT_SOURCE_DIR}/cmake/rodaq.map"
    )
    target_link_options(rodaq PRIVATE
        "LINKER:--exclude-libs,ALL"
        "LINKER:--version-script=${PROJECT_SOURCE_DIR}/cmake/rodaq.map"
    )
endif()
rodaq_enable_warnings(rodaq)
rodaq_enable_sanitizers(rodaq)

if((BUILD_TESTING AND RODAQ_BUILD_TESTS) OR RODAQ_BUILD_BENCHMARKS)
    add_library(rodaq_testing STATIC $<TARGET_OBJECTS:rodaq_objects>)
    target_compile_features(rodaq_testing PUBLIC cxx_std_17)
    target_include_directories(rodaq_testing
        PUBLIC
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(rodaq_testing
        PUBLIC
            rodaq_plugin
            rodaq_sdk
            fmt::fmt
            spdlog::spdlog
        PRIVATE
            yaml-cpp::yaml-cpp
            hdr_histogram::hdr_histogram_core_static
    )
    set_target_properties(rodaq_testing PROPERTIES POSITION_INDEPENDENT_CODE ON)
    rodaq_enable_warnings(rodaq_testing)
    rodaq_enable_sanitizers(rodaq_testing)
endif()
