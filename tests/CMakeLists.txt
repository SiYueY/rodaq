add_executable(rodaq_result_test result_test.cpp)
target_link_libraries(rodaq_result_test PRIVATE rodaq::plugin)
add_test(NAME rodaq_result_test COMMAND rodaq_result_test)

add_executable(rodaq_logging_test logging_test.cpp)
target_link_libraries(rodaq_logging_test PRIVATE rodaq::plugin)
add_test(NAME rodaq_logging_test COMMAND rodaq_logging_test)

add_executable(rodaq_public_headers_test public_headers_test.cpp)
target_link_libraries(rodaq_public_headers_test PRIVATE rodaq::plugin)
add_test(NAME rodaq_public_headers_test COMMAND rodaq_public_headers_test)

add_executable(rodaq_logging_async_file_test logging_async_file_test.cpp)
target_link_libraries(rodaq_logging_async_file_test PRIVATE rodaq::plugin)
add_test(NAME rodaq_logging_async_file_test COMMAND rodaq_logging_async_file_test)

add_executable(rodaq_logging_compile_level_test logging_compile_level_test.cpp)
target_link_libraries(rodaq_logging_compile_level_test PRIVATE rodaq::plugin)
add_test(NAME rodaq_logging_compile_level_test COMMAND rodaq_logging_compile_level_test)

# Tests use assertions even in release builds.
get_property(test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS test_targets)
    target_compile_options(${target} PRIVATE -UNDEBUG)
    rodaq_enable_project_warnings(${target})
endforeach()

file(GLOB_RECURSE public_cpp_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/rodaq/*.hpp)
foreach(header IN LISTS public_cpp_headers)
    file(RELATIVE_PATH include_path ${PROJECT_SOURCE_DIR}/include ${header})
    string(MAKE_C_IDENTIFIER "${include_path}" header_id)
    set(source ${CMAKE_CURRENT_BINARY_DIR}/${header_id}.cpp)
    file(GENERATE OUTPUT ${source} CONTENT "#include <${include_path}>\n")
    add_library(${header_id} OBJECT ${source})
    target_link_libraries(${header_id} PRIVATE rodaq::plugin)
    rodaq_enable_project_warnings(${header_id})
endforeach()

find_package(GTest 1.11 REQUIRED)
add_executable(rodaq_build_contract_test build_contract_test.cpp)
target_link_libraries(rodaq_build_contract_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_build_contract_test)
include(GoogleTest)
add_executable(rodaq_task_config_test task/config_test.cpp)
target_include_directories(rodaq_task_config_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(rodaq_task_config_test PRIVATE RODAQ_TASK_CONFIG_FIXTURE="${PROJECT_SOURCE_DIR}/tests/task/fixtures/continuous.json")
target_link_libraries(rodaq_task_config_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_task_config_test)
gtest_discover_tests(rodaq_task_config_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_task_identity_test task/identity_test.cpp)
target_include_directories(rodaq_task_identity_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(rodaq_task_identity_test PRIVATE RODAQ_TASK_CONFIG_FIXTURE="${PROJECT_SOURCE_DIR}/tests/task/fixtures/continuous.json")
target_link_libraries(rodaq_task_identity_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_task_identity_test)
gtest_discover_tests(rodaq_task_identity_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_task_create_store_test task/create_store_test.cpp)
target_include_directories(rodaq_task_create_store_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_task_create_store_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_task_create_store_test)
gtest_discover_tests(rodaq_task_create_store_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_task_operation_history_test task/operation_history_test.cpp)
target_include_directories(rodaq_task_operation_history_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_task_operation_history_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_task_operation_history_test)
gtest_discover_tests(rodaq_task_operation_history_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_task_control_record_test task/control_record_test.cpp)
target_include_directories(rodaq_task_control_record_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_task_control_record_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_task_control_record_test)
gtest_discover_tests(rodaq_task_control_record_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_task_store_test task/task_store_test.cpp)
target_include_directories(rodaq_task_store_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(rodaq_task_store_test PRIVATE RODAQ_TASK_CONFIG_FIXTURE="${PROJECT_SOURCE_DIR}/tests/task/fixtures/continuous.json")
target_link_libraries(rodaq_task_store_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_task_store_test)
gtest_discover_tests(rodaq_task_store_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_task_controller_test task/task_controller_test.cpp)
target_include_directories(rodaq_task_controller_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(rodaq_task_controller_test PRIVATE RODAQ_TASK_CONFIG_FIXTURE="${PROJECT_SOURCE_DIR}/tests/task/fixtures/continuous.json")
target_link_libraries(rodaq_task_controller_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_task_controller_test)
gtest_discover_tests(rodaq_task_controller_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_task_recovery_test task/recovery_test.cpp)
target_include_directories(rodaq_task_recovery_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_task_recovery_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_task_recovery_test)
gtest_discover_tests(rodaq_task_recovery_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_task_config_identity_test task/config_identity_test.cpp)
target_include_directories(rodaq_task_config_identity_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(rodaq_task_config_identity_test PRIVATE RODAQ_TASK_CONFIG_FIXTURE="${PROJECT_SOURCE_DIR}/tests/task/fixtures/continuous.json")
target_link_libraries(rodaq_task_config_identity_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_task_config_identity_test)
gtest_discover_tests(rodaq_task_config_identity_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_stream_identity_test pipeline/stream_identity_test.cpp)
target_include_directories(rodaq_stream_identity_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_stream_identity_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_stream_identity_test)
gtest_discover_tests(rodaq_stream_identity_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_sequence_allocator_test pipeline/sequence_allocator_test.cpp)
target_include_directories(rodaq_sequence_allocator_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_sequence_allocator_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_sequence_allocator_test)
gtest_discover_tests(rodaq_sequence_allocator_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_admission_gate_test pipeline/admission_gate_test.cpp)
target_include_directories(rodaq_admission_gate_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_admission_gate_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_admission_gate_test)
gtest_discover_tests(rodaq_admission_gate_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_payload_pool_test pipeline/payload_pool_test.cpp)
target_include_directories(rodaq_payload_pool_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_payload_pool_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_payload_pool_test)
gtest_discover_tests(rodaq_payload_pool_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_descriptor_registry_test pipeline/descriptor_registry_test.cpp)
target_include_directories(rodaq_descriptor_registry_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_descriptor_registry_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_descriptor_registry_test)
gtest_discover_tests(rodaq_descriptor_registry_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_record_queue_test pipeline/record_queue_test.cpp)
target_include_directories(rodaq_record_queue_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_record_queue_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_record_queue_test)
gtest_discover_tests(rodaq_record_queue_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_volume_capacity_test artifact/volume_capacity_test.cpp)
target_include_directories(rodaq_volume_capacity_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_volume_capacity_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_volume_capacity_test)
gtest_discover_tests(rodaq_volume_capacity_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_task_config_probe task/config_probe.cpp)
target_link_libraries(rodaq_task_config_probe PRIVATE rodaq::daq)
rodaq_enable_project_warnings(rodaq_task_config_probe)
add_executable(rodaq_persistent_json_test persistence/json_test.cpp)
target_include_directories(rodaq_persistent_json_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_persistent_json_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_persistent_json_test)
gtest_discover_tests(rodaq_persistent_json_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_atomic_file_test persistence/atomic_file_test.cpp)
target_include_directories(rodaq_atomic_file_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_atomic_file_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_atomic_file_test)
gtest_discover_tests(rodaq_atomic_file_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_clock_test time/clock_test.cpp)
target_include_directories(rodaq_clock_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_clock_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_clock_test)
gtest_discover_tests(rodaq_clock_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_playback_anchor_store_test time/playback_anchor_store_test.cpp)
target_include_directories(rodaq_playback_anchor_store_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_playback_anchor_store_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_playback_anchor_store_test)
gtest_discover_tests(rodaq_playback_anchor_store_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_clock_correlation_test time/clock_correlation_test.cpp)
target_include_directories(rodaq_clock_correlation_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_clock_correlation_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_clock_correlation_test)
gtest_discover_tests(rodaq_clock_correlation_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_jcs_number_probe persistence/number_probe.cpp)
target_include_directories(rodaq_jcs_number_probe PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_jcs_number_probe PRIVATE rodaq::daq nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_jcs_number_probe)
gtest_discover_tests(rodaq_build_contract_test)
add_executable(rodaq_abi_boundary_test plugin/abi_boundary_test.cpp)
target_link_libraries(rodaq_abi_boundary_test PRIVATE rodaq::plugin GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_abi_boundary_test)
gtest_discover_tests(rodaq_abi_boundary_test)
add_executable(rodaq_plugin_authoring_test plugin/authoring_test.cpp)
target_link_libraries(rodaq_plugin_authoring_test PRIVATE rodaq::plugin GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_plugin_authoring_test)
gtest_discover_tests(rodaq_plugin_authoring_test)
add_executable(rodaq_plugin_catalog_test plugin/catalog_test.cpp)
target_include_directories(rodaq_plugin_catalog_test PRIVATE ${PROJECT_SOURCE_DIR}/src/plugin)
target_link_libraries(rodaq_plugin_catalog_test PRIVATE rodaq::plugin GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_plugin_catalog_test)
gtest_discover_tests(rodaq_plugin_catalog_test)
add_executable(rodaq_plugin_registry_test plugin/registry_test.cpp)
target_include_directories(rodaq_plugin_registry_test PRIVATE ${PROJECT_SOURCE_DIR}/src/plugin)
target_link_libraries(rodaq_plugin_registry_test PRIVATE rodaq::plugin GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_plugin_registry_test)
gtest_discover_tests(rodaq_plugin_registry_test PROPERTIES TIMEOUT 30)
add_executable(rodaq_plugin_initialization_test plugin/initialization_test.cpp)
target_include_directories(rodaq_plugin_initialization_test PRIVATE ${PROJECT_SOURCE_DIR}/src/plugin)
target_link_libraries(rodaq_plugin_initialization_test PRIVATE rodaq::plugin GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_plugin_initialization_test)
gtest_discover_tests(rodaq_plugin_initialization_test PROPERTIES TIMEOUT 30)

add_executable(rodaq_plugin_runtime_test plugin/runtime_test.cpp)
target_link_libraries(rodaq_plugin_runtime_test PRIVATE rodaq::plugin GTest::gtest_main nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
target_compile_definitions(rodaq_plugin_runtime_test PRIVATE
    RODAQ_ACQUISITION_PACKAGE="${PROJECT_BINARY_DIR}/counter-matrix/acquisition_provider-0-installed/lib/rodaq/plugins/com.example.acquisition.probe/1.0.0"
    RODAQ_STORAGE_PACKAGE="${PROJECT_BINARY_DIR}/counter-matrix/storage_provider-0-installed/lib/rodaq/plugins/com.example.storage.probe/1.0.0"
    RODAQ_LIFECYCLE_PACKAGE="${PROJECT_BINARY_DIR}/counter-matrix/lifecycle_provider-0-installed/lib/rodaq/plugins/com.example.lifecycle/1.0.0"
    RODAQ_COUNTER_PACKAGE="${PROJECT_BINARY_DIR}/counter-matrix/cpp_provider-0-installed/lib/rodaq/plugins/com.example.counter/1.0.0")
rodaq_enable_project_warnings(rodaq_plugin_runtime_test)
gtest_discover_tests(rodaq_plugin_runtime_test PROPERTIES TIMEOUT 40 FIXTURES_REQUIRED counter_sdk)

# Invalid bootstrap fixtures exercise the Loader, never the production SDK registration path.
add_library(rodaq_loader_fault_provider SHARED plugin/loader_fault_provider.cpp)
target_include_directories(rodaq_loader_fault_provider PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(rodaq_loader_fault_provider PROPERTIES CXX_VISIBILITY_PRESET hidden)
rodaq_enable_project_warnings(rodaq_loader_fault_provider)
foreach(own_version RANGE 0 1)
    add_library(rodaq_loader_dependency_${own_version} SHARED plugin/loader_dependency_proxy.cpp)
    target_include_directories(rodaq_loader_dependency_${own_version} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(rodaq_loader_dependency_${own_version} PRIVATE OWN_VERSION=${own_version})
    target_link_libraries(rodaq_loader_dependency_${own_version} PRIVATE rodaq_loader_fault_provider)
    set_target_properties(rodaq_loader_dependency_${own_version} PROPERTIES BUILD_RPATH_USE_ORIGIN TRUE)
    rodaq_enable_project_warnings(rodaq_loader_dependency_${own_version})
endforeach()
add_executable(rodaq_loader_fault_consumer plugin/loader_fault_consumer.cpp)
target_link_libraries(rodaq_loader_fault_consumer PRIVATE rodaq::plugin)
rodaq_enable_project_warnings(rodaq_loader_fault_consumer)
add_executable(rodaq_loader_supervision_consumer plugin/loader_supervision_consumer.cpp)
target_link_libraries(rodaq_loader_supervision_consumer PRIVATE rodaq::plugin)
rodaq_enable_project_warnings(rodaq_loader_supervision_consumer)
add_library(rodaq_catalog_block_interposer SHARED plugin/catalog_block_interposer.c)
target_link_libraries(rodaq_catalog_block_interposer PRIVATE ${CMAKE_DL_LIBS})
rodaq_enable_project_warnings(rodaq_catalog_block_interposer)
add_executable(rodaq_catalog_supervision_consumer plugin/catalog_supervision_consumer.cpp)
target_link_libraries(rodaq_catalog_supervision_consumer PRIVATE rodaq::plugin)
rodaq_enable_project_warnings(rodaq_catalog_supervision_consumer)

# Clang 14 TSan owns strong global new/delete symbols. Allocation-failure
# injection uses replacements, so run this case in native and ASan/UBSan builds.
if(NOT RODAQ_SANITIZER STREQUAL "thread")
    add_executable(rodaq_close_takeover_test plugin/close_takeover_test.cpp)
    target_link_libraries(rodaq_close_takeover_test PRIVATE rodaq::plugin GTest::gtest_main)
    target_compile_definitions(rodaq_close_takeover_test PRIVATE
        RODAQ_LIFECYCLE_PACKAGE="${PROJECT_BINARY_DIR}/counter-matrix/lifecycle_provider-0-installed/lib/rodaq/plugins/com.example.lifecycle/1.0.0")
    rodaq_enable_project_warnings(rodaq_close_takeover_test)
    gtest_discover_tests(rodaq_close_takeover_test PROPERTIES TIMEOUT 40 FIXTURES_REQUIRED counter_sdk)
endif()

add_executable(rodaq_acquisition_sdk_test acquisition/sdk_test.cpp)
target_link_libraries(rodaq_acquisition_sdk_test PRIVATE rodaq::plugin GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_acquisition_sdk_test)
gtest_discover_tests(rodaq_acquisition_sdk_test PROPERTIES TIMEOUT 30)

add_executable(rodaq_storage_sdk_test storage/sdk_test.cpp)
target_link_libraries(rodaq_storage_sdk_test PRIVATE rodaq::plugin GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_storage_sdk_test)
gtest_discover_tests(rodaq_storage_sdk_test PROPERTIES TIMEOUT 30)

# Compile every C ABI header alone in both supported language modes.
file(GLOB_RECURSE abi_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/rodaq/*/c/*.h)
foreach(header IN LISTS abi_headers)
    file(RELATIVE_PATH include_path ${PROJECT_SOURCE_DIR}/include ${header})
    string(MAKE_C_IDENTIFIER "${include_path}" header_id)
    foreach(language c cpp)
        set(source ${CMAKE_CURRENT_BINARY_DIR}/${header_id}.${language})
        file(GENERATE OUTPUT ${source} CONTENT "#include <${include_path}>\n")
        add_library(${header_id}_${language} OBJECT ${source})
        target_include_directories(${header_id}_${language} PRIVATE ${PROJECT_SOURCE_DIR}/include)
        rodaq_enable_project_warnings(${header_id}_${language})
    endforeach()
endforeach()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(RODAQ_SCHEMA_PYTHON_EXECUTABLE "" CACHE FILEPATH "Prepared jsonschema 4.25.1 qualification interpreter")
if(RODAQ_SCHEMA_PYTHON_EXECUTABLE)
    add_test(NAME rodaq_task_schema_conformance
        COMMAND ${RODAQ_SCHEMA_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/task/schema_test.py
            --schema ${PROJECT_SOURCE_DIR}/schemas/task_config.schema.json
            --fixtures ${PROJECT_SOURCE_DIR}/tests/task/fixtures
            --probe $<TARGET_FILE:rodaq_task_config_probe>)
    set_tests_properties(rodaq_task_schema_conformance PROPERTIES TIMEOUT 60)
endif()
add_test(NAME rodaq_catalog_supervision
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/plugin/catalog_supervision_test.py
        --interposer $<TARGET_FILE:rodaq_catalog_block_interposer>
        --consumer $<TARGET_FILE:rodaq_catalog_supervision_consumer>)
set_tests_properties(rodaq_catalog_supervision PROPERTIES TIMEOUT 45)
add_test(NAME rodaq_loader_bootstrap_rejection
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/plugin/loader_fault_test.py
        --provider $<TARGET_FILE:rodaq_loader_fault_provider>
        --consumer $<TARGET_FILE:rodaq_loader_fault_consumer>
        --proxy $<TARGET_FILE:rodaq_loader_dependency_0> --proxy $<TARGET_FILE:rodaq_loader_dependency_1>
        --manifest ${PROJECT_BINARY_DIR}/counter-matrix/cpp_provider-0-installed/lib/rodaq/plugins/com.example.counter/1.0.0/manifest.json)
set_tests_properties(rodaq_loader_bootstrap_rejection PROPERTIES TIMEOUT 120 FIXTURES_REQUIRED counter_sdk)
add_test(NAME rodaq_loader_supervision
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/plugin/loader_supervision_test.py
        --provider $<TARGET_FILE:rodaq_loader_fault_provider>
        --consumer $<TARGET_FILE:rodaq_loader_supervision_consumer>
        --manifest ${PROJECT_BINARY_DIR}/counter-matrix/cpp_provider-0-installed/lib/rodaq/plugins/com.example.counter/1.0.0/manifest.json
        --healthy-package ${PROJECT_BINARY_DIR}/counter-matrix/lifecycle_provider-0-installed/lib/rodaq/plugins/com.example.lifecycle/1.0.0)
set_tests_properties(rodaq_loader_supervision PROPERTIES TIMEOUT 45 FIXTURES_REQUIRED counter_sdk)
string(TOUPPER "${CMAKE_BUILD_TYPE}" rodaq_build_type)
add_custom_target(rodaq_generate_abi_probe
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/abi_probe.c
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/generate_abi_probe.py
        --root ${PROJECT_SOURCE_DIR} --output ${CMAKE_CURRENT_BINARY_DIR}/abi_probe.c
        "--flags=${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${rodaq_build_type}} $<JOIN:$<TARGET_PROPERTY:rodaq_abi_probe,COMPILE_OPTIONS>, > -std=c11; position_independent=${CMAKE_POSITION_INDEPENDENT_CODE}"
    DEPENDS ${abi_headers} ${PROJECT_SOURCE_DIR}/tools/generate_abi_probe.py
    VERBATIM)
add_executable(rodaq_abi_probe ${CMAKE_CURRENT_BINARY_DIR}/abi_probe.c)
add_dependencies(rodaq_abi_probe rodaq_generate_abi_probe)
target_include_directories(rodaq_abi_probe PRIVATE ${PROJECT_SOURCE_DIR}/include)
rodaq_enable_project_warnings(rodaq_abi_probe)
add_library(rodaq_abi_signatures OBJECT abi_signatures.c)
target_include_directories(rodaq_abi_signatures PRIVATE ${PROJECT_SOURCE_DIR}/include)
rodaq_enable_project_warnings(rodaq_abi_signatures)
configure_file(abi_signatures.c ${CMAKE_CURRENT_BINARY_DIR}/abi_signatures.cpp COPYONLY)
add_library(rodaq_abi_signatures_cpp OBJECT ${CMAKE_CURRENT_BINARY_DIR}/abi_signatures.cpp)
target_include_directories(rodaq_abi_signatures_cpp PRIVATE ${PROJECT_SOURCE_DIR}/include)
rodaq_enable_project_warnings(rodaq_abi_signatures_cpp)
add_test(NAME rodaq_abi_manifest_comparator
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/abi_manifest_test.py)
add_test(NAME rodaq_plugin_metadata
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/plugin_metadata_test.py)
add_test(NAME rodaq_counter_binary_matrix
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/plugin/abi/run_counter_matrix.py
        --source ${PROJECT_SOURCE_DIR} --build ${PROJECT_BINARY_DIR}
        --compiler ${CMAKE_C_COMPILER} --cxx-compiler ${CMAKE_CXX_COMPILER}
        --sanitizer ${RODAQ_SANITIZER})
set_tests_properties(rodaq_counter_binary_matrix PROPERTIES FIXTURES_SETUP counter_sdk)

add_test(NAME rodaq_domain_binary_matrix
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/plugin/abi/run_domain_matrix.py
        --source ${PROJECT_SOURCE_DIR} --build ${PROJECT_BINARY_DIR}
        --compiler ${CMAKE_C_COMPILER} --sanitizer ${RODAQ_SANITIZER})

set_tests_properties(rodaq_domain_binary_matrix PROPERTIES FIXTURES_SETUP domain_sdk)
add_test(NAME rodaq_runtime_sdk_consumer
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/plugin/abi/build_runtime_consumer.py
        --source ${PROJECT_SOURCE_DIR} --build ${PROJECT_BINARY_DIR}
        --compiler ${CMAKE_CXX_COMPILER} --sanitizer ${RODAQ_SANITIZER})
set_tests_properties(rodaq_runtime_sdk_consumer PROPERTIES FIXTURES_REQUIRED "counter_sdk;domain_sdk")
add_test(NAME rodaq_counter_independent_projects
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/plugin/abi/run_counter_projects.py
        --source ${PROJECT_SOURCE_DIR} --build ${PROJECT_BINARY_DIR}
        --compiler ${CMAKE_CXX_COMPILER} --sanitizer ${RODAQ_SANITIZER})

add_test(NAME rodaq_package_contract
    COMMAND ${CMAKE_COMMAND}
        -DRODAQ_BUILD_DIR=${PROJECT_BINARY_DIR}
        -DRODAQ_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DRODAQ_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DRODAQ_SANITIZER=${RODAQ_SANITIZER}
        -P ${PROJECT_SOURCE_DIR}/tests/package_contract.cmake)

if(RODAQ_SANITIZER STREQUAL "address-undefined")
    add_executable(rodaq_provider_lifetime_violation plugin/provider_lifetime_violation.cpp)
    target_link_libraries(rodaq_provider_lifetime_violation PRIVATE rodaq::plugin)
    target_compile_options(rodaq_provider_lifetime_violation PRIVATE -UNDEBUG)
    rodaq_enable_project_warnings(rodaq_provider_lifetime_violation)
    add_test(NAME rodaq_provider_lifetime_qualification
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/expected_sanitizer_failure.py
            --binary $<TARGET_FILE:rodaq_provider_lifetime_violation>
            --report ${PROJECT_BINARY_DIR}/provider-lifetime-report.log)
    set_tests_properties(rodaq_provider_lifetime_qualification PROPERTIES
        TIMEOUT 30 ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=1:halt_on_error=1")
endif()

add_executable(rodaq_segment_capacity_test pipeline/segment_capacity_test.cpp)
target_include_directories(rodaq_segment_capacity_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_segment_capacity_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_segment_capacity_test)
gtest_discover_tests(rodaq_segment_capacity_test PROPERTIES TIMEOUT 30)

add_executable(rodaq_admission_test pipeline/admission_test.cpp)
target_include_directories(rodaq_admission_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_admission_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_admission_test)
gtest_discover_tests(rodaq_admission_test PROPERTIES TIMEOUT 30)

if(NOT RODAQ_SANITIZER STREQUAL "thread")
    target_sources(rodaq_admission_test PRIVATE pipeline/admission_allocation.cpp)
    target_compile_definitions(rodaq_admission_test PRIVATE RODAQ_TEST_ADMISSION_ALLOCATION=1)
endif()

add_executable(rodaq_segment_id_store_test artifact/segment_id_store_test.cpp)
target_include_directories(rodaq_segment_id_store_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_segment_id_store_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_segment_id_store_test)
gtest_discover_tests(rodaq_segment_id_store_test PROPERTIES TIMEOUT 30)

add_executable(rodaq_managed_volume_test artifact/managed_volume_test.cpp)
target_include_directories(rodaq_managed_volume_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_managed_volume_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_managed_volume_test)
gtest_discover_tests(rodaq_managed_volume_test PROPERTIES TIMEOUT 30)

add_executable(rodaq_sealed_files_test artifact/sealed_files_test.cpp)
target_include_directories(rodaq_sealed_files_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_sealed_files_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_sealed_files_test)
gtest_discover_tests(rodaq_sealed_files_test PROPERTIES TIMEOUT 30)

add_executable(rodaq_ledger_test artifact/ledger_test.cpp)
target_include_directories(rodaq_ledger_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_ledger_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_ledger_test)
gtest_discover_tests(rodaq_ledger_test PROPERTIES TIMEOUT 30)

add_executable(rodaq_manifest_test artifact/manifest_test.cpp)
target_include_directories(rodaq_manifest_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_manifest_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_manifest_test)
gtest_discover_tests(rodaq_manifest_test PROPERTIES TIMEOUT 30)

add_executable(rodaq_segment_metadata_test pipeline/segment_metadata_test.cpp)
target_include_directories(rodaq_segment_metadata_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_segment_metadata_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_segment_metadata_test)
gtest_discover_tests(rodaq_segment_metadata_test PROPERTIES TIMEOUT 30)

add_executable(rodaq_segment_committer_test artifact/segment_committer_test.cpp)
target_include_directories(rodaq_segment_committer_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_segment_committer_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_segment_committer_test)
gtest_discover_tests(rodaq_segment_committer_test PROPERTIES TIMEOUT 30)

if(RODAQ_BUILD_BEHAVIORAL_FAKES)
    add_executable(rodaq_fake_storage_hang_probe fakes/storage_hang_probe.cpp)
    target_link_libraries(rodaq_fake_storage_hang_probe PRIVATE rodaq::plugin)
    rodaq_enable_project_warnings(rodaq_fake_storage_hang_probe)
    add_executable(rodaq_fake_storage_test fakes/storage_test.cpp)
    target_link_libraries(rodaq_fake_storage_test PRIVATE rodaq::plugin GTest::gtest_main Threads::Threads)
    target_compile_definitions(rodaq_fake_storage_test PRIVATE
        RODAQ_FAKE_STORAGE_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_storage/rodaq-plugins"
        RODAQ_FAKE_STORAGE_HANG_PROBE="$<TARGET_FILE:rodaq_fake_storage_hang_probe>")
    add_dependencies(rodaq_fake_storage_test rodaq_fake_storage rodaq_fake_storage_hang_probe)
    rodaq_enable_project_warnings(rodaq_fake_storage_test)
    gtest_discover_tests(rodaq_fake_storage_test PROPERTIES TIMEOUT 30)
endif()

if(RODAQ_BUILD_BEHAVIORAL_FAKES)
    add_executable(rodaq_pipeline_writer_test pipeline/writer_test.cpp)
    target_include_directories(rodaq_pipeline_writer_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(rodaq_pipeline_writer_test PRIVATE rodaq::daq GTest::gtest_main Threads::Threads)
    target_compile_definitions(rodaq_pipeline_writer_test PRIVATE
        RODAQ_WRITER_STORAGE_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_storage/rodaq-plugins")
    add_dependencies(rodaq_pipeline_writer_test rodaq_fake_storage)
    rodaq_enable_project_warnings(rodaq_pipeline_writer_test)
    gtest_discover_tests(rodaq_pipeline_writer_test PROPERTIES TIMEOUT 30)
endif()

if(RODAQ_BUILD_BEHAVIORAL_FAKES)
    add_executable(rodaq_fake_acquisition_test fakes/acquisition_test.cpp)
    target_link_libraries(rodaq_fake_acquisition_test PRIVATE rodaq::plugin GTest::gtest_main nlohmann_json::nlohmann_json Threads::Threads)
    target_compile_definitions(rodaq_fake_acquisition_test PRIVATE
        RODAQ_FAKE_ACQUISITION_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_acquisition/rodaq-plugins")
    add_dependencies(rodaq_fake_acquisition_test rodaq_fake_acquisition)
    rodaq_enable_project_warnings(rodaq_fake_acquisition_test)
    gtest_discover_tests(rodaq_fake_acquisition_test PROPERTIES TIMEOUT 30)
endif()

add_executable(rodaq_stream_table_test pipeline/stream_table_test.cpp)
target_include_directories(rodaq_stream_table_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_stream_table_test PRIVATE rodaq::daq GTest::gtest_main Threads::Threads)
rodaq_enable_project_warnings(rodaq_stream_table_test)
gtest_discover_tests(rodaq_stream_table_test PROPERTIES TIMEOUT 30)

if(RODAQ_BUILD_BEHAVIORAL_FAKES)
    add_executable(rodaq_recording_instance_test pipeline/recording_instance_test.cpp)
    target_include_directories(rodaq_recording_instance_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(rodaq_recording_instance_test PRIVATE rodaq::daq GTest::gtest_main Threads::Threads)
    target_compile_definitions(rodaq_recording_instance_test PRIVATE
        RODAQ_INSTANCE_STORAGE_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_storage/rodaq-plugins"
        RODAQ_INSTANCE_ACQUISITION_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_acquisition/rodaq-plugins")
    add_dependencies(rodaq_recording_instance_test rodaq_fake_storage rodaq_fake_acquisition)
    rodaq_enable_project_warnings(rodaq_recording_instance_test)
    gtest_discover_tests(rodaq_recording_instance_test PROPERTIES TIMEOUT 30)
    add_executable(rodaq_task_service_crash_probe task/service_crash_probe.cpp)
    target_include_directories(rodaq_task_service_crash_probe PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(rodaq_task_service_crash_probe PRIVATE rodaq::daq nlohmann_json::nlohmann_json)
    target_compile_definitions(rodaq_task_service_crash_probe PRIVATE RODAQ_TASK_CONFIG_FIXTURE="${PROJECT_SOURCE_DIR}/tests/task/fixtures/continuous.json")
    rodaq_enable_project_warnings(rodaq_task_service_crash_probe)
    add_executable(rodaq_task_service_test task/task_service_test.cpp)
    target_include_directories(rodaq_task_service_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(rodaq_task_service_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json Threads::Threads)
    target_compile_definitions(rodaq_task_service_test PRIVATE
        RODAQ_TASK_CONFIG_FIXTURE="${PROJECT_SOURCE_DIR}/tests/task/fixtures/continuous.json"
        RODAQ_SERVICE_CRASH_PROBE="$<TARGET_FILE:rodaq_task_service_crash_probe>"
        RODAQ_SERVICE_STORAGE_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_storage/rodaq-plugins"
        RODAQ_SERVICE_ACQUISITION_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_acquisition/rodaq-plugins")
    add_dependencies(rodaq_task_service_test rodaq_task_service_crash_probe rodaq_fake_storage rodaq_fake_acquisition)
    rodaq_enable_project_warnings(rodaq_task_service_test)
    gtest_discover_tests(rodaq_task_service_test PROPERTIES TIMEOUT 30)
endif()
