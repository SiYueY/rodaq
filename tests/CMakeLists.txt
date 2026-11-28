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
gtest_discover_tests(rodaq_build_contract_test)

if(TARGET rodaq_ros2_config)
    add_executable(rodaq_ros2_runtime_test ros2/runtime_test.cpp ros2/core_test.cpp)
    target_link_libraries(rodaq_ros2_runtime_test PRIVATE rodaq_ros2_runtime GTest::gtest_main
        nlohmann_json::nlohmann_json)
    target_compile_definitions(rodaq_ros2_runtime_test PRIVATE
        RODAQ_ROS2_PACKAGE="${PROJECT_BINARY_DIR}/plugins/acquisition/ros2/rodaq-plugins")
    add_dependencies(rodaq_ros2_runtime_test rodaq_ros2_acquisition)
    if(TARGET rodaq_mcap_format)
        find_package(sensor_msgs REQUIRED)
        include(ExternalProject)
        get_filename_component(rodaq_ros2_fixture_ros_prefix "${sensor_msgs_DIR}/../../.." ABSOLUTE)
        set(rodaq_ros2_custom_prefix "${CMAKE_CURRENT_BINARY_DIR}/ros2-custom-overlay/install")
        ExternalProject_Add(rodaq_ros2_custom_overlay
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ros2/custom_overlay"
            BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/ros2-custom-overlay/build"
            INSTALL_DIR "${rodaq_ros2_custom_prefix}"
            DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
            BUILD_ALWAYS TRUE
            CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
                -DCMAKE_PREFIX_PATH=${rodaq_ros2_fixture_ros_prefix}
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                -DCMAKE_CXX_STANDARD=17 -DCMAKE_BUILD_TYPE=Release
                -DPython3_EXECUTABLE=${Python3_EXECUTABLE} -DBUILD_TESTING=OFF
            BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 2)
        add_dependencies(rodaq_ros2_runtime_test rodaq_ros2_custom_overlay)
        target_sources(rodaq_ros2_runtime_test PRIVATE ros2/robot_test.cpp)
        target_link_libraries(rodaq_ros2_runtime_test PRIVATE
            sensor_msgs::sensor_msgs__rosidl_typesupport_cpp OpenSSL::Crypto)
        target_link_libraries(rodaq_ros2_runtime_test PRIVATE rodaq::daq rodaq_mcap_format)
        target_include_directories(rodaq_ros2_runtime_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(rodaq_ros2_runtime_test PRIVATE
            RODAQ_MCAP_PACKAGE="${PROJECT_BINARY_DIR}/plugins/storage/mcap/rodaq-plugins")
        add_dependencies(rodaq_ros2_runtime_test rodaq_mcap_storage)
    endif()
    rodaq_enable_project_warnings(rodaq_ros2_runtime_test)
    gtest_discover_tests(rodaq_ros2_runtime_test DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)
    if(TARGET rodaq_ros2_custom_overlay)
        configure_file(ros2/overlay_environment.cmake.in
            "${CMAKE_CURRENT_BINARY_DIR}/ros2-overlay-environment.cmake" @ONLY)
        set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
            "${CMAKE_CURRENT_BINARY_DIR}/ros2-overlay-environment.cmake")
    endif()
    add_executable(rodaq_ros2_support_test ros2/support_test.cpp)
    target_link_libraries(rodaq_ros2_support_test PRIVATE rodaq_ros2_support GTest::gtest_main)
    rodaq_enable_project_warnings(rodaq_ros2_support_test)
    gtest_discover_tests(rodaq_ros2_support_test PROPERTIES TIMEOUT 30)
    add_executable(rodaq_ros2_config_test ros2/config_test.cpp)
    target_link_libraries(rodaq_ros2_config_test PRIVATE rodaq_ros2_config GTest::gtest_main
        nlohmann_json::nlohmann_json)
    target_compile_definitions(rodaq_ros2_config_test PRIVATE
        RODAQ_ROS2_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/ros2/fixtures")
    rodaq_enable_project_warnings(rodaq_ros2_config_test)
    gtest_discover_tests(rodaq_ros2_config_test PROPERTIES TIMEOUT 30)
endif()

if(TARGET rodaq_mcap_format)
    add_executable(rodaq_mcap_storage_test mcap/storage_test.cpp)
    target_link_libraries(rodaq_mcap_storage_test PRIVATE rodaq_mcap_format GTest::gtest_main)
    target_compile_definitions(rodaq_mcap_storage_test PRIVATE
        RODAQ_MCAP_PACKAGE="${PROJECT_BINARY_DIR}/plugins/storage/mcap/rodaq-plugins")
    add_dependencies(rodaq_mcap_storage_test rodaq_mcap_storage)
    rodaq_enable_project_warnings(rodaq_mcap_storage_test)
    gtest_discover_tests(rodaq_mcap_storage_test PROPERTIES TIMEOUT 30)
endif()

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

add_executable(rodaq_task_operation_history_test task/operation_history_test.cpp)
target_include_directories(rodaq_task_operation_history_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_task_operation_history_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_task_operation_history_test)
gtest_discover_tests(rodaq_task_operation_history_test PROPERTIES TIMEOUT 30)

add_executable(rodaq_task_store_test task/task_store_test.cpp)
target_include_directories(rodaq_task_store_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(rodaq_task_store_test PRIVATE RODAQ_TASK_CONFIG_FIXTURE="${PROJECT_SOURCE_DIR}/tests/task/fixtures/continuous.json")
target_link_libraries(rodaq_task_store_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json)
rodaq_enable_project_warnings(rodaq_task_store_test)
gtest_discover_tests(rodaq_task_store_test PROPERTIES TIMEOUT 30)

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

if(RODAQ_BUILD_MCAP_STORAGE OR RODAQ_BUILD_ROS2_ACQUISITION)
    add_test(NAME rodaq_oot_official_plugins_contract
        COMMAND ${CMAKE_COMMAND}
            -DRODAQ_BUILD_DIR=${PROJECT_BINARY_DIR}
            -DRODAQ_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DRODAQ_C_COMPILER=${CMAKE_C_COMPILER}
            -DRODAQ_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DRODAQ_BUILD_MCAP_STORAGE=${RODAQ_BUILD_MCAP_STORAGE}
            -DRODAQ_BUILD_ROS2_ACQUISITION=${RODAQ_BUILD_ROS2_ACQUISITION}
            -P ${PROJECT_SOURCE_DIR}/tests/oot_plugins_contract.cmake)
    set_tests_properties(rodaq_oot_official_plugins_contract PROPERTIES TIMEOUT 120)
endif()

add_executable(rodaq_admission_test pipeline/admission_test.cpp)
target_include_directories(rodaq_admission_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_admission_test PRIVATE rodaq::daq GTest::gtest_main)
rodaq_enable_project_warnings(rodaq_admission_test)
gtest_discover_tests(rodaq_admission_test PROPERTIES TIMEOUT 30)

if(NOT RODAQ_SANITIZER STREQUAL "thread")
    target_sources(rodaq_admission_test PRIVATE pipeline/admission_allocation.cpp)
    target_compile_definitions(rodaq_admission_test PRIVATE RODAQ_TEST_ADMISSION_ALLOCATION=1)
endif()

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

    add_executable(rodaq_pipeline_writer_test pipeline/writer_test.cpp)
    target_include_directories(rodaq_pipeline_writer_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(rodaq_pipeline_writer_test PRIVATE rodaq::daq GTest::gtest_main Threads::Threads)
    target_compile_definitions(rodaq_pipeline_writer_test PRIVATE
        RODAQ_WRITER_STORAGE_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_storage/rodaq-plugins")
    add_dependencies(rodaq_pipeline_writer_test rodaq_fake_storage)
    rodaq_enable_project_warnings(rodaq_pipeline_writer_test)
    gtest_discover_tests(rodaq_pipeline_writer_test PROPERTIES TIMEOUT 30)

    add_executable(rodaq_fake_acquisition_test fakes/acquisition_test.cpp)
    target_link_libraries(rodaq_fake_acquisition_test PRIVATE rodaq::plugin GTest::gtest_main nlohmann_json::nlohmann_json Threads::Threads)
    target_compile_definitions(rodaq_fake_acquisition_test PRIVATE
        RODAQ_FAKE_ACQUISITION_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_acquisition/rodaq-plugins")
    add_dependencies(rodaq_fake_acquisition_test rodaq_fake_acquisition)
    rodaq_enable_project_warnings(rodaq_fake_acquisition_test)
    gtest_discover_tests(rodaq_fake_acquisition_test PROPERTIES TIMEOUT 30)

    add_executable(rodaq_recording_instance_test pipeline/recording_instance_test.cpp)
    target_include_directories(rodaq_recording_instance_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(rodaq_recording_instance_test PRIVATE rodaq::daq GTest::gtest_main Threads::Threads)
    target_compile_definitions(rodaq_recording_instance_test PRIVATE
        RODAQ_INSTANCE_STORAGE_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_storage/rodaq-plugins"
        RODAQ_INSTANCE_ACQUISITION_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_acquisition/rodaq-plugins")
    add_dependencies(rodaq_recording_instance_test rodaq_fake_storage rodaq_fake_acquisition)
    rodaq_enable_project_warnings(rodaq_recording_instance_test)
    gtest_discover_tests(rodaq_recording_instance_test PROPERTIES TIMEOUT 30)

    add_executable(rodaq_pipeline_controller_test pipeline/pipeline_controller_test.cpp)
    target_include_directories(rodaq_pipeline_controller_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(rodaq_pipeline_controller_test PRIVATE rodaq::daq GTest::gtest_main Threads::Threads)
    target_compile_definitions(rodaq_pipeline_controller_test PRIVATE
        RODAQ_PIPELINE_STORAGE_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_storage/rodaq-plugins"
        RODAQ_PIPELINE_LIFECYCLE_PACKAGE="${PROJECT_BINARY_DIR}/counter-matrix/lifecycle_provider-0-installed/lib/rodaq/plugins/com.example.lifecycle/1.0.0"
        RODAQ_PIPELINE_ACQUISITION_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_acquisition/rodaq-plugins")
    add_dependencies(rodaq_pipeline_controller_test rodaq_fake_storage rodaq_fake_acquisition)
    rodaq_enable_project_warnings(rodaq_pipeline_controller_test)
    gtest_discover_tests(rodaq_pipeline_controller_test PROPERTIES TIMEOUT 30)
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/pipeline/cleanup_fixture.cmake")

    add_executable(rodaq_task_service_test task/task_service_test.cpp)
    target_include_directories(rodaq_task_service_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(rodaq_task_service_test PRIVATE rodaq::daq GTest::gtest_main nlohmann_json::nlohmann_json Threads::Threads)
    target_compile_definitions(rodaq_task_service_test PRIVATE
        RODAQ_TASK_CONFIG_FIXTURE="${PROJECT_SOURCE_DIR}/tests/task/fixtures/continuous.json"
        RODAQ_SERVICE_STORAGE_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_storage/rodaq-plugins"
        RODAQ_SERVICE_ACQUISITION_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_acquisition/rodaq-plugins")
    add_dependencies(rodaq_task_service_test rodaq_fake_storage rodaq_fake_acquisition)
    rodaq_enable_project_warnings(rodaq_task_service_test)
    gtest_discover_tests(rodaq_task_service_test PROPERTIES TIMEOUT 30)

    add_executable(rodaq_task_reset_retire_test task/reset_retire_test.cpp)
    target_include_directories(rodaq_task_reset_retire_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(rodaq_task_reset_retire_test PRIVATE rodaq::daq GTest::gtest_main Threads::Threads)
    target_compile_definitions(rodaq_task_reset_retire_test PRIVATE
        RODAQ_RESET_STORAGE_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_storage/rodaq-plugins"
        RODAQ_RESET_ACQUISITION_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_acquisition/rodaq-plugins")
    add_dependencies(rodaq_task_reset_retire_test rodaq_fake_storage rodaq_fake_acquisition)
    rodaq_enable_project_warnings(rodaq_task_reset_retire_test)
    gtest_discover_tests(rodaq_task_reset_retire_test PROPERTIES TIMEOUT 30)

    # Intentionally omits src/ include dir to prove public-header-only rodaq::daq consumption.
    add_executable(rodaq_system_service_test system/system_service_test.cpp)
    target_link_libraries(rodaq_system_service_test PRIVATE rodaq::daq GTest::gtest_main Threads::Threads)
    target_compile_definitions(rodaq_system_service_test PRIVATE
        RODAQ_SYSTEM_STORAGE_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_storage/rodaq-plugins"
        RODAQ_SYSTEM_ACQUISITION_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_acquisition/rodaq-plugins")
    add_dependencies(rodaq_system_service_test rodaq_fake_storage rodaq_fake_acquisition)
    rodaq_enable_project_warnings(rodaq_system_service_test)
    gtest_discover_tests(rodaq_system_service_test PROPERTIES TIMEOUT 30)
endif()

add_executable(rodaq_stream_table_test pipeline/stream_table_test.cpp)
target_include_directories(rodaq_stream_table_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rodaq_stream_table_test PRIVATE rodaq::daq GTest::gtest_main Threads::Threads)
rodaq_enable_project_warnings(rodaq_stream_table_test)
gtest_discover_tests(rodaq_stream_table_test PROPERTIES TIMEOUT 30)
