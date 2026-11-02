set(RODAQ_FIXTURE_SUPPORT_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fixture_support.cpp"
)

function(rodaq_add_plugin_fixture target source)
    add_library("${target}" SHARED
        "${source}"
        "${RODAQ_FIXTURE_SUPPORT_SOURCE}"
    )
    target_link_libraries("${target}" PRIVATE rodaq_headers)
    set_target_properties("${target}" PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
    rodaq_enable_warnings("${target}")
    rodaq_enable_sanitizers("${target}")
endfunction()

set(RODAQ_ACQUISITION_FIXTURE_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/acquisition_fixture.cpp"
)
set(RODAQ_STORAGE_FIXTURE_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/storage_fixture.cpp"
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_valid_acquisition
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
rodaq_add_plugin_fixture(
    rodaq_test_plugin_valid_storage
    "${RODAQ_STORAGE_FIXTURE_SOURCE}"
)
rodaq_add_plugin_fixture(
    rodaq_test_plugin_missing_boot_marker
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_missing_boot_marker
    PRIVATE RODAQ_FIXTURE_DEFINE_MARKER=0
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_wrong_boot_abi
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_wrong_boot_abi
    PRIVATE RODAQ_FIXTURE_BOOT_ABI_VERSION=2
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_missing_family
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_missing_family
    PRIVATE RODAQ_FIXTURE_DEFINE_FAMILY=0
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_dual_family
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_dual_family
    PRIVATE RODAQ_FIXTURE_DUAL_FAMILY=1
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_wrong_protocol
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_wrong_protocol
    PRIVATE RODAQ_FIXTURE_PROTOCOL_VERSION=2
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_too_small_family
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_too_small_family
    PRIVATE RODAQ_FIXTURE_PROTOCOL_STRUCT_SIZE=71
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_missing_slot
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_missing_slot
    PRIVATE RODAQ_FIXTURE_CREATE_FUNCTION=nullptr
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_larger_appendable
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_larger_appendable
    PRIVATE RODAQ_FIXTURE_PROTOCOL_STRUCT_SIZE=80
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_null_descriptor
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_null_descriptor
    PRIVATE RODAQ_FIXTURE_DESCRIPTOR_NULL=1
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_small_descriptor
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_small_descriptor
    PRIVATE RODAQ_FIXTURE_DESCRIPTOR_STRUCT_SIZE=71
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_invalid_id
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_invalid_id
    PRIVATE "RODAQ_FIXTURE_PLUGIN_ID=\"Rodaq.invalid\""
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_invalid_version
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_invalid_version
    PRIVATE "RODAQ_FIXTURE_PLUGIN_VERSION=\"01.0.0\""
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_invalid_utf8
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_invalid_utf8
    PRIVATE RODAQ_FIXTURE_INVALID_UTF8_NAME=1
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_create_failure
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_create_failure
    PRIVATE RODAQ_FIXTURE_CREATE_FUNCTION=rodaq::test::fixture_create_failure
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_create_null
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_create_null
    PRIVATE RODAQ_FIXTURE_CREATE_FUNCTION=rodaq::test::fixture_create_null
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_duplicate_id
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_storage_wrong_protocol
    "${RODAQ_STORAGE_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_storage_wrong_protocol
    PRIVATE RODAQ_FIXTURE_PROTOCOL_VERSION=2
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_storage_missing_slot
    "${RODAQ_STORAGE_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_storage_missing_slot
    PRIVATE RODAQ_FIXTURE_CREATE_FUNCTION=nullptr
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_marker_provider
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/symbol_provider.cpp"
)
target_compile_definitions(
    rodaq_test_plugin_marker_provider
    PRIVATE RODAQ_PROVIDER_DEFINE_FAMILY=0
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_entrypoint_provider
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/symbol_provider.cpp"
)
target_compile_definitions(
    rodaq_test_plugin_entrypoint_provider
    PRIVATE RODAQ_PROVIDER_DEFINE_MARKER=0
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_provenance_marker
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_provenance_marker
    PRIVATE RODAQ_FIXTURE_DEFINE_MARKER=0
)
target_link_options(
    rodaq_test_plugin_provenance_marker
    PRIVATE LINKER:--no-as-needed
)
target_link_libraries(
    rodaq_test_plugin_provenance_marker
    PRIVATE rodaq_test_plugin_marker_provider
)

rodaq_add_plugin_fixture(
    rodaq_test_plugin_provenance_entrypoint
    "${RODAQ_ACQUISITION_FIXTURE_SOURCE}"
)
target_compile_definitions(
    rodaq_test_plugin_provenance_entrypoint
    PRIVATE RODAQ_FIXTURE_DEFINE_FAMILY=0
)
target_link_options(
    rodaq_test_plugin_provenance_entrypoint
    PRIVATE LINKER:--no-as-needed
)
target_link_libraries(
    rodaq_test_plugin_provenance_entrypoint
    PRIVATE rodaq_test_plugin_entrypoint_provider
)

add_library(rodaq_test_plugin_plain SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/plain_library.cpp"
)
set_target_properties(rodaq_test_plugin_plain PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
rodaq_enable_warnings(rodaq_test_plugin_plain)
rodaq_enable_sanitizers(rodaq_test_plugin_plain)

add_executable(rodaq_plugin_validation_test plugin_validation_test.cpp)
target_link_libraries(rodaq_plugin_validation_test
    PRIVATE
        GTest::gtest_main
        rodaq_plugin
)
rodaq_enable_warnings(rodaq_plugin_validation_test)
rodaq_enable_sanitizers(rodaq_plugin_validation_test)
add_test(NAME rodaq.plugin.validation COMMAND rodaq_plugin_validation_test)

add_executable(rodaq_service_registry_test service_registry_test.cpp)
target_link_libraries(rodaq_service_registry_test
    PRIVATE
        GTest::gtest_main
        rodaq_plugin
)
rodaq_enable_warnings(rodaq_service_registry_test)
rodaq_enable_sanitizers(rodaq_service_registry_test)
add_test(NAME rodaq.plugin.service_registry COMMAND rodaq_service_registry_test)

add_executable(rodaq_config_view_test config_view_test.cpp)
target_link_libraries(rodaq_config_view_test
    PRIVATE
        GTest::gtest_main
        rodaq_plugin
)
rodaq_enable_warnings(rodaq_config_view_test)
rodaq_enable_sanitizers(rodaq_config_view_test)
add_test(NAME rodaq.plugin.config_view COMMAND rodaq_config_view_test)

add_executable(
    rodaq_plugin_service_integration_test
    plugin_service_integration_test.cpp
)
target_link_libraries(rodaq_plugin_service_integration_test
    PRIVATE
        GTest::gtest_main
        rodaq_plugin
        rodaq_runtime
)
rodaq_enable_warnings(rodaq_plugin_service_integration_test)
rodaq_enable_sanitizers(rodaq_plugin_service_integration_test)
add_dependencies(
    rodaq_plugin_service_integration_test
    rodaq_test_plugin_valid_acquisition
    rodaq_test_plugin_valid_storage
)
add_test(
    NAME rodaq.plugin.service_integration
    COMMAND rodaq_plugin_service_integration_test
)
set_tests_properties(
    rodaq.plugin.service_integration
    PROPERTIES
        ENVIRONMENT
            "RODAQ_TEST_ACQUISITION_PLUGIN=$<TARGET_FILE:rodaq_test_plugin_valid_acquisition>;RODAQ_TEST_STORAGE_PLUGIN=$<TARGET_FILE:rodaq_test_plugin_valid_storage>"
)

add_executable(rodaq_plugin_loader_probe plugin_loader_probe.cpp)
target_link_libraries(rodaq_plugin_loader_probe PRIVATE rodaq_plugin)
rodaq_enable_warnings(rodaq_plugin_loader_probe)
rodaq_enable_sanitizers(rodaq_plugin_loader_probe)

set(RODAQ_PLUGIN_EVENT_DIR "${CMAKE_CURRENT_BINARY_DIR}/events")
file(MAKE_DIRECTORY "${RODAQ_PLUGIN_EVENT_DIR}")

function(rodaq_add_plugin_probe_test test_name)
    add_test(
        NAME "rodaq.plugin.${test_name}"
        COMMAND rodaq_plugin_loader_probe ${ARGN}
    )
endfunction()

rodaq_add_plugin_probe_test(
    shared_library
    shared_library
    "${RODAQ_PLUGIN_EVENT_DIR}/shared_library.log"
    "$<TARGET_FILE:rodaq_test_plugin_valid_acquisition>"
    "$<TARGET_FILE:rodaq_test_plugin_plain>"
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/plain_library.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/missing-plugin.so"
)

rodaq_add_plugin_probe_test(
    valid_libraries
    validate_success
    "${RODAQ_PLUGIN_EVENT_DIR}/valid_libraries.log"
    "$<TARGET_FILE:rodaq_test_plugin_valid_acquisition>"
    "$<TARGET_FILE:rodaq_test_plugin_valid_storage>"
    "$<TARGET_FILE:rodaq_test_plugin_larger_appendable>"
)

rodaq_add_plugin_probe_test(
    missing_boot_marker
    validate_error_without_entrypoint
    3
    "$<TARGET_FILE:rodaq_test_plugin_missing_boot_marker>"
    "${RODAQ_PLUGIN_EVENT_DIR}/missing_boot_marker.log"
)

rodaq_add_plugin_probe_test(
    wrong_boot_abi
    validate_error_without_entrypoint
    1
    "$<TARGET_FILE:rodaq_test_plugin_wrong_boot_abi>"
    "${RODAQ_PLUGIN_EVENT_DIR}/wrong_boot_abi.log"
)

rodaq_add_plugin_probe_test(
    missing_family
    validate_error_without_entrypoint
    3
    "$<TARGET_FILE:rodaq_test_plugin_missing_family>"
    "${RODAQ_PLUGIN_EVENT_DIR}/missing_family.log"
)

rodaq_add_plugin_probe_test(
    dual_family
    validate_error_without_entrypoint
    1
    "$<TARGET_FILE:rodaq_test_plugin_dual_family>"
    "${RODAQ_PLUGIN_EVENT_DIR}/dual_family.log"
)

rodaq_add_plugin_probe_test(
    wrong_protocol
    validate_error
    1
    "$<TARGET_FILE:rodaq_test_plugin_wrong_protocol>"
)

rodaq_add_plugin_probe_test(
    too_small_family
    validate_error
    1
    "$<TARGET_FILE:rodaq_test_plugin_too_small_family>"
)

rodaq_add_plugin_probe_test(
    missing_slot
    validate_error
    1
    "$<TARGET_FILE:rodaq_test_plugin_missing_slot>"
)

rodaq_add_plugin_probe_test(
    null_descriptor
    validate_error
    1
    "$<TARGET_FILE:rodaq_test_plugin_null_descriptor>"
)

rodaq_add_plugin_probe_test(
    small_descriptor
    validate_error
    1
    "$<TARGET_FILE:rodaq_test_plugin_small_descriptor>"
)

rodaq_add_plugin_probe_test(
    invalid_plugin_id
    validate_error
    1
    "$<TARGET_FILE:rodaq_test_plugin_invalid_id>"
)

rodaq_add_plugin_probe_test(
    invalid_plugin_version
    validate_error
    1
    "$<TARGET_FILE:rodaq_test_plugin_invalid_version>"
)

rodaq_add_plugin_probe_test(
    invalid_plugin_utf8
    validate_error
    1
    "$<TARGET_FILE:rodaq_test_plugin_invalid_utf8>"
)

rodaq_add_plugin_probe_test(
    storage_wrong_protocol
    validate_error
    1
    "$<TARGET_FILE:rodaq_test_plugin_storage_wrong_protocol>"
)

rodaq_add_plugin_probe_test(
    storage_missing_slot
    validate_error
    1
    "$<TARGET_FILE:rodaq_test_plugin_storage_missing_slot>"
)

rodaq_add_plugin_probe_test(
    catalog_conflict
    catalog_conflict
    "$<TARGET_FILE:rodaq_test_plugin_valid_acquisition>"
    "$<TARGET_FILE:rodaq_test_plugin_duplicate_id>"
)

rodaq_add_plugin_probe_test(
    family_mismatch
    family_mismatch
    "$<TARGET_FILE:rodaq_test_plugin_valid_acquisition>"
    "$<TARGET_FILE:rodaq_test_plugin_valid_storage>"
    "${RODAQ_PLUGIN_EVENT_DIR}/family_mismatch.log"
)

rodaq_add_plugin_probe_test(
    catalog_path_merge
    catalog_path_merge
    "$<TARGET_FILE:rodaq_test_plugin_valid_acquisition>"
    "${RODAQ_PLUGIN_EVENT_DIR}/catalog_path_merge.log"
)

rodaq_add_plugin_probe_test(
    catalog_remove_instance_lifetime
    catalog_remove_instance_lifetime
    "$<TARGET_FILE:rodaq_test_plugin_valid_acquisition>"
    "${RODAQ_PLUGIN_EVENT_DIR}/catalog_remove_instance_lifetime.log"
)

rodaq_add_plugin_probe_test(
    catalog_remove_library_lifetime
    catalog_remove_library_lifetime
    "$<TARGET_FILE:rodaq_test_plugin_valid_acquisition>"
    "${RODAQ_PLUGIN_EVENT_DIR}/catalog_remove_library_lifetime.log"
)

rodaq_add_plugin_probe_test(
    library_lease
    library_lease
    "$<TARGET_FILE:rodaq_test_plugin_valid_acquisition>"
    "${RODAQ_PLUGIN_EVENT_DIR}/library_lease.log"
)

rodaq_add_plugin_probe_test(
    acquisition_plugin_instance
    acquisition_plugin_instance
    "$<TARGET_FILE:rodaq_test_plugin_valid_acquisition>"
    "${RODAQ_PLUGIN_EVENT_DIR}/acquisition_plugin_instance.log"
)

rodaq_add_plugin_probe_test(
    storage_plugin_instance
    storage_plugin_instance
    "$<TARGET_FILE:rodaq_test_plugin_valid_storage>"
    "${RODAQ_PLUGIN_EVENT_DIR}/storage_plugin_instance.log"
)

rodaq_add_plugin_probe_test(
    descriptor_lifetime
    descriptor_lifetime
    "$<TARGET_FILE:rodaq_test_plugin_valid_acquisition>"
    "${RODAQ_PLUGIN_EVENT_DIR}/descriptor_lifetime.log"
)

rodaq_add_plugin_probe_test(
    create_failure
    create_failure
    "$<TARGET_FILE:rodaq_test_plugin_create_failure>"
)

rodaq_add_plugin_probe_test(
    create_null
    create_null
    "$<TARGET_FILE:rodaq_test_plugin_create_null>"
)

rodaq_add_plugin_probe_test(
    quarantine
    quarantine
    "$<TARGET_FILE:rodaq_test_plugin_valid_acquisition>"
    "${RODAQ_PLUGIN_EVENT_DIR}/quarantine.log"
)

rodaq_add_plugin_probe_test(
    provenance_marker
    provenance
    "$<TARGET_FILE:rodaq_test_plugin_provenance_marker>"
    "${RODAQ_PLUGIN_EVENT_DIR}/provenance_marker.log"
)

rodaq_add_plugin_probe_test(
    provenance_entrypoint
    provenance
    "$<TARGET_FILE:rodaq_test_plugin_provenance_entrypoint>"
    "${RODAQ_PLUGIN_EVENT_DIR}/provenance_entrypoint.log"
)

add_dependencies(
    rodaq_plugin_loader_probe
    rodaq_test_plugin_valid_acquisition
    rodaq_test_plugin_valid_storage
    rodaq_test_plugin_missing_boot_marker
    rodaq_test_plugin_wrong_boot_abi
    rodaq_test_plugin_missing_family
    rodaq_test_plugin_dual_family
    rodaq_test_plugin_wrong_protocol
    rodaq_test_plugin_too_small_family
    rodaq_test_plugin_missing_slot
    rodaq_test_plugin_larger_appendable
    rodaq_test_plugin_null_descriptor
    rodaq_test_plugin_small_descriptor
    rodaq_test_plugin_invalid_id
    rodaq_test_plugin_invalid_version
    rodaq_test_plugin_invalid_utf8
    rodaq_test_plugin_create_failure
    rodaq_test_plugin_create_null
    rodaq_test_plugin_duplicate_id
    rodaq_test_plugin_storage_wrong_protocol
    rodaq_test_plugin_storage_missing_slot
    rodaq_test_plugin_marker_provider
    rodaq_test_plugin_entrypoint_provider
    rodaq_test_plugin_provenance_marker
    rodaq_test_plugin_provenance_entrypoint
    rodaq_test_plugin_plain
)
