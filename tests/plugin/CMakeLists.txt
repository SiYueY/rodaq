add_library(rodaq_plugin_test_support INTERFACE)
target_include_directories(
    rodaq_plugin_test_support
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
        ${PROJECT_SOURCE_DIR}/src/plugin)
target_link_libraries(rodaq_plugin_test_support INTERFACE rodaq::core)

add_executable(plugin_test_support_self_test support/assertion_self_test.cpp)
target_link_libraries(plugin_test_support_self_test PRIVATE rodaq_plugin_test_support)
rodaq_enable_project_warnings(plugin_test_support_self_test)
add_test(NAME plugin_test_support_self_test COMMAND plugin_test_support_self_test)
add_test(
    NAME plugin_test_support_failure_test
    COMMAND plugin_test_support_self_test --expect-failure)
set_tests_properties(plugin_test_support_failure_test PROPERTIES WILL_FAIL TRUE)

add_executable(plugin_abi_c_test abi/plugin_abi_c_test.c)
target_link_libraries(plugin_abi_c_test PRIVATE rodaq_plugin_test_support)
rodaq_enable_project_warnings(plugin_abi_c_test)
add_test(NAME plugin_abi_c_test COMMAND plugin_abi_c_test)

add_executable(plugin_abi_cpp_test abi/plugin_abi_cpp_test.cpp)
target_link_libraries(plugin_abi_cpp_test PRIVATE rodaq_plugin_test_support)
rodaq_enable_project_warnings(plugin_abi_cpp_test)
add_test(NAME plugin_abi_cpp_test COMMAND plugin_abi_cpp_test)

add_executable(plugin_abi_support_test abi/plugin_abi_support_test.cpp)
target_include_directories(
    plugin_abi_support_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/plugin)
target_link_libraries(
    plugin_abi_support_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
rodaq_enable_project_warnings(plugin_abi_support_test)
add_test(NAME plugin_abi_support_test COMMAND plugin_abi_support_test)

foreach(header IN ITEMS deadline interface service_registry plugin manager)
    add_library(
        plugin_header_${header}_compile
        OBJECT
        abi/header_${header}_test.cpp)
    target_link_libraries(
        plugin_header_${header}_compile
        PRIVATE
            rodaq_plugin_authoring)
    rodaq_enable_project_warnings(plugin_header_${header}_compile)
endforeach()

add_executable(
    plugin_counter_sdk_test
    counter/counter_sdk_test.cpp
    counter/counter_plugin.cpp)
target_include_directories(
    plugin_counter_sdk_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/plugin)
target_link_libraries(
    plugin_counter_sdk_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_counter_sdk_test
    PRIVATE
        RODAQ_PLUGIN_ID="com.rodaq.test.counter"
        RODAQ_PLUGIN_NAME="Counter SDK Test"
        RODAQ_PLUGIN_VERSION="1.0.0")
rodaq_enable_project_warnings(plugin_counter_sdk_test)
add_test(NAME plugin_counter_sdk_test COMMAND plugin_counter_sdk_test)

add_library(plugin_counter_fixture SHARED counter/counter_plugin.cpp)
target_include_directories(
    plugin_counter_fixture
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/counter)
target_compile_definitions(
    plugin_counter_fixture
    PRIVATE
        RODAQ_PLUGIN_ID="com.rodaq.test.counter"
        RODAQ_PLUGIN_NAME="Counter SDK Test"
        RODAQ_PLUGIN_VERSION="1.0.0")
rodaq_register_plugin(
    plugin_counter_fixture
    PLUGIN_ID com.rodaq.test.counter
    PLUGIN_NAME "Counter SDK Test"
    PLUGIN_VERSION "1.0.0")

add_executable(plugin_manifest_test loader/plugin_manifest_test.cpp)
target_include_directories(
    plugin_manifest_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/plugin)
target_link_libraries(
    plugin_manifest_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
rodaq_enable_project_warnings(plugin_manifest_test)
add_test(NAME plugin_manifest_test COMMAND plugin_manifest_test)

add_executable(plugin_catalog_loader_test loader/plugin_catalog_loader_test.cpp)
target_include_directories(
    plugin_catalog_loader_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/plugin)
target_link_libraries(
    plugin_catalog_loader_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_catalog_loader_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}"
        RODAQ_TEST_COUNTER_PACKAGE="${RODAQ_PLUGIN_PACKAGE_ROOT}/plugin_counter_fixture")
add_dependencies(plugin_catalog_loader_test plugin_counter_fixture)
rodaq_enable_project_warnings(plugin_catalog_loader_test)
add_test(NAME plugin_catalog_loader_test COMMAND plugin_catalog_loader_test)

add_executable(plugin_lifecycle_test lifecycle/plugin_lifecycle_test.cpp)
target_include_directories(
    plugin_lifecycle_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/counter)
target_link_libraries(
    plugin_lifecycle_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_lifecycle_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}"
        RODAQ_TEST_COUNTER_ARTIFACT="${RODAQ_PLUGIN_PACKAGE_ROOT}/plugin_counter_fixture/lib/libplugin_counter_fixture.so")
add_dependencies(plugin_lifecycle_test plugin_counter_fixture)
rodaq_enable_project_warnings(plugin_lifecycle_test)
add_test(NAME plugin_lifecycle_test COMMAND plugin_lifecycle_test)

add_library(plugin_slow_configure_fixture SHARED fixtures/slow_configure_plugin.cpp)
rodaq_register_plugin(
    plugin_slow_configure_fixture
    PLUGIN_ID com.rodaq.test.slow-configure
    PLUGIN_NAME "Slow Configure Test"
    PLUGIN_VERSION "1.0.0")

add_library(plugin_hung_create_fixture SHARED fixtures/hung_create_plugin.cpp)
rodaq_register_plugin(
    plugin_hung_create_fixture
    PLUGIN_ID com.rodaq.test.hung-create
    PLUGIN_NAME "Hung Create Test"
    PLUGIN_VERSION "1.0.0")

add_library(plugin_slow_shutdown_fixture SHARED fixtures/slow_shutdown_plugin.cpp)
rodaq_register_plugin(
    plugin_slow_shutdown_fixture
    PLUGIN_ID com.rodaq.test.slow-shutdown
    PLUGIN_NAME "Slow Shutdown Test"
    PLUGIN_VERSION "1.0.0")

add_library(plugin_hung_shutdown_fixture SHARED fixtures/hung_shutdown_plugin.cpp)
rodaq_register_plugin(
    plugin_hung_shutdown_fixture
    PLUGIN_ID com.rodaq.test.hung-shutdown
    PLUGIN_NAME "Hung Shutdown Test"
    PLUGIN_VERSION "1.0.0")

add_library(plugin_shutdown_failure_fixture SHARED fixtures/shutdown_failure_plugin.cpp)
rodaq_register_plugin(
    plugin_shutdown_failure_fixture
    PLUGIN_ID com.rodaq.test.shutdown-failure
    PLUGIN_NAME "Shutdown Failure Test"
    PLUGIN_VERSION "1.0.0")

add_library(plugin_malformed_create_fixture SHARED fixtures/malformed_create_plugin.cpp)
rodaq_register_plugin(
    plugin_malformed_create_fixture
    PLUGIN_ID com.rodaq.test.malformed-create
    PLUGIN_NAME "Malformed Create Test"
    PLUGIN_VERSION "1.0.0")

add_library(plugin_invalid_success_fixture SHARED fixtures/invalid_success_plugin.cpp)
rodaq_register_plugin(
    plugin_invalid_success_fixture
    PLUGIN_ID com.rodaq.test.invalid-success
    PLUGIN_NAME "Invalid Success Test"
    PLUGIN_VERSION "1.0.0")

add_library(
    plugin_partial_create_failure_fixture
    SHARED
    fixtures/partial_create_failure_plugin.cpp)
rodaq_register_plugin(
    plugin_partial_create_failure_fixture
    PLUGIN_ID com.rodaq.test.partial-create-failure
    PLUGIN_NAME "Partial Create Failure Test"
    PLUGIN_VERSION "1.0.0")

add_library(
    plugin_provenance_dependency
    SHARED
    fixtures/provenance_dependency.cpp)
target_link_libraries(
    plugin_provenance_dependency
    PRIVATE
        rodaq_plugin_authoring)
rodaq_enable_project_warnings(plugin_provenance_dependency)

add_library(
    plugin_missing_entry_provenance_fixture
    SHARED
    fixtures/missing_entry_provenance_plugin.cpp)
target_link_libraries(
    plugin_missing_entry_provenance_fixture
    PRIVATE
        plugin_provenance_dependency)
rodaq_register_plugin(
    plugin_missing_entry_provenance_fixture
    PLUGIN_ID com.rodaq.test.missing-entry-provenance
    PLUGIN_NAME "Missing Entry Provenance Test"
    PLUGIN_VERSION "1.0.0")

add_executable(plugin_supervision_test lifecycle/plugin_supervision_test.cpp)
target_link_libraries(
    plugin_supervision_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_supervision_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
add_dependencies(
    plugin_supervision_test
    plugin_counter_fixture
    plugin_slow_configure_fixture
    plugin_hung_create_fixture)
rodaq_enable_project_warnings(plugin_supervision_test)
add_test(NAME plugin_supervision_test COMMAND plugin_supervision_test)

add_executable(plugin_hung_configure_test lifecycle/plugin_hung_configure_test.cpp)
target_link_libraries(
    plugin_hung_configure_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_hung_configure_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
add_dependencies(plugin_hung_configure_test plugin_slow_configure_fixture)
rodaq_enable_project_warnings(plugin_hung_configure_test)
add_test(NAME plugin_hung_configure_test COMMAND plugin_hung_configure_test)

add_executable(plugin_close_future_test lifecycle/plugin_close_future_test.cpp)
target_link_libraries(
    plugin_close_future_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_close_future_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
add_dependencies(plugin_close_future_test plugin_slow_shutdown_fixture)
rodaq_enable_project_warnings(plugin_close_future_test)
add_test(NAME plugin_close_future_test COMMAND plugin_close_future_test)

add_executable(
    plugin_close_concurrency_test
    lifecycle/plugin_close_concurrency_test.cpp)
target_link_libraries(
    plugin_close_concurrency_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_close_concurrency_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
add_dependencies(
    plugin_close_concurrency_test
    plugin_hung_shutdown_fixture
    plugin_slow_shutdown_fixture)
rodaq_enable_project_warnings(plugin_close_concurrency_test)
add_test(
    NAME plugin_close_concurrency_test
    COMMAND plugin_close_concurrency_test)

add_executable(
    plugin_create_shutdown_test
    lifecycle/plugin_create_shutdown_test.cpp)
target_link_libraries(
    plugin_create_shutdown_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_create_shutdown_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
add_dependencies(
    plugin_create_shutdown_test
    plugin_slow_configure_fixture)
rodaq_enable_project_warnings(plugin_create_shutdown_test)
add_test(
    NAME plugin_create_shutdown_test
    COMMAND plugin_create_shutdown_test)

add_executable(
    plugin_shutdown_failure_test
    lifecycle/plugin_shutdown_failure_test.cpp)
target_link_libraries(
    plugin_shutdown_failure_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_shutdown_failure_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
add_dependencies(
    plugin_shutdown_failure_test
    plugin_shutdown_failure_fixture)
rodaq_enable_project_warnings(plugin_shutdown_failure_test)
add_test(
    NAME plugin_shutdown_failure_test
    COMMAND plugin_shutdown_failure_test)

add_executable(
    plugin_faulted_library_test
    lifecycle/plugin_faulted_library_test.cpp)
target_link_libraries(
    plugin_faulted_library_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_faulted_library_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
add_dependencies(
    plugin_faulted_library_test
    plugin_malformed_create_fixture)
rodaq_enable_project_warnings(plugin_faulted_library_test)
add_test(
    NAME plugin_faulted_library_test
    COMMAND plugin_faulted_library_test)

add_executable(
    plugin_invalid_success_test
    lifecycle/plugin_invalid_success_test.cpp)
target_link_libraries(
    plugin_invalid_success_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_invalid_success_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
add_dependencies(plugin_invalid_success_test plugin_invalid_success_fixture)
rodaq_enable_project_warnings(plugin_invalid_success_test)
add_test(
    NAME plugin_invalid_success_test
    COMMAND plugin_invalid_success_test)

add_executable(
    plugin_create_failure_output_test
    lifecycle/plugin_create_failure_output_test.cpp)
target_link_libraries(
    plugin_create_failure_output_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_create_failure_output_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
add_dependencies(
    plugin_create_failure_output_test
    plugin_partial_create_failure_fixture)
rodaq_enable_project_warnings(plugin_create_failure_output_test)
add_test(
    NAME plugin_create_failure_output_test
    COMMAND plugin_create_failure_output_test)

add_executable(plugin_refresh_test lifecycle/plugin_refresh_test.cpp)
target_include_directories(
    plugin_refresh_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/counter)
target_link_libraries(
    plugin_refresh_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_refresh_test
    PRIVATE
        RODAQ_TEST_COUNTER_PACKAGE="${RODAQ_PLUGIN_PACKAGE_ROOT}/plugin_counter_fixture")
add_dependencies(plugin_refresh_test plugin_counter_fixture)
rodaq_enable_project_warnings(plugin_refresh_test)
add_test(NAME plugin_refresh_test COMMAND plugin_refresh_test)

add_library(plugin_configure_failure_fixture SHARED fixtures/configure_failure_plugin.cpp)
rodaq_register_plugin(
    plugin_configure_failure_fixture
    PLUGIN_ID com.rodaq.test.configure-failure
    PLUGIN_NAME "Configure Failure Test"
    PLUGIN_VERSION "1.0.0")

add_library(
    plugin_async_service_callback_fixture
    SHARED
    fixtures/async_service_callback_plugin.cpp)
target_include_directories(
    plugin_async_service_callback_fixture
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/counter)
rodaq_register_plugin(
    plugin_async_service_callback_fixture
    PLUGIN_ID com.rodaq.test.async-service-callback
    PLUGIN_NAME "Async Service Callback Test"
    PLUGIN_VERSION "1.0.0")

add_library(
    plugin_unknown_status_fixture
    SHARED
    fixtures/unknown_operation_status_plugin.cpp)
rodaq_register_plugin(
    plugin_unknown_status_fixture
    PLUGIN_ID com.rodaq.test.unknown-operation-status
    PLUGIN_NAME "Unknown Operation Status Test"
    PLUGIN_VERSION "1.0.0")

add_executable(
    plugin_configure_failure_test
    lifecycle/plugin_configure_failure_test.cpp)
target_link_libraries(
    plugin_configure_failure_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_configure_failure_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
add_dependencies(
    plugin_configure_failure_test
    plugin_configure_failure_fixture)
rodaq_enable_project_warnings(plugin_configure_failure_test)
add_test(
    NAME plugin_configure_failure_test
    COMMAND plugin_configure_failure_test)

add_executable(
    plugin_async_service_callback_test
    lifecycle/plugin_async_service_callback_test.cpp)
target_include_directories(
    plugin_async_service_callback_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/counter)
target_link_libraries(
    plugin_async_service_callback_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_async_service_callback_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
add_dependencies(
    plugin_async_service_callback_test
    plugin_async_service_callback_fixture)
rodaq_enable_project_warnings(plugin_async_service_callback_test)
add_test(
    NAME plugin_async_service_callback_test
    COMMAND plugin_async_service_callback_test)

add_executable(
    plugin_unknown_status_test
    lifecycle/plugin_unknown_status_test.cpp)
target_link_libraries(
    plugin_unknown_status_test
    PRIVATE
        rodaq_plugin_test_support
        rodaq_plugin_runtime)
target_compile_definitions(
    plugin_unknown_status_test
    PRIVATE
        RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
add_dependencies(
    plugin_unknown_status_test
    plugin_unknown_status_fixture)
rodaq_enable_project_warnings(plugin_unknown_status_test)
add_test(
    NAME plugin_unknown_status_test
    COMMAND plugin_unknown_status_test)

add_test(
    NAME plugin_framework_isolation_test
    COMMAND
        ${CMAKE_COMMAND}
        -DRODAQ_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DRODAQ_RUNTIME_LINK_FILE=${CMAKE_BINARY_DIR}/CMakeFiles/rodaq_plugin_runtime.dir/link.txt
        -P ${CMAKE_CURRENT_SOURCE_DIR}/framework/framework_isolation_test.cmake)

add_test(
    NAME plugin_public_sdk_surface_test
    COMMAND
        ${CMAKE_COMMAND}
        -DRODAQ_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/framework/public_sdk_surface_test.cmake)

add_test(
    NAME plugin_manifest_generator_test
    COMMAND
        ${CMAKE_COMMAND}
        -DRODAQ_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/framework/manifest_generator_test.cmake)

if(TARGET rodaq_mcap_reference_plugin)
    find_package(mcap_vendor REQUIRED)
    add_executable(
        plugin_reference_mcap_test
        reference/mcap_reference_test.cpp)
    target_include_directories(
        plugin_reference_mcap_test
        PRIVATE
            ${PROJECT_SOURCE_DIR}/plugin/mcap_storage/include)
    target_link_libraries(
        plugin_reference_mcap_test
        PRIVATE
            rodaq_plugin_test_support
            rodaq_plugin_runtime
            mcap_vendor::mcap)
    target_compile_definitions(
        plugin_reference_mcap_test
        PRIVATE
            RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
    add_dependencies(plugin_reference_mcap_test rodaq_mcap_reference_plugin)
    rodaq_enable_project_warnings(plugin_reference_mcap_test)
    add_test(NAME plugin_reference_mcap_test COMMAND plugin_reference_mcap_test)
endif()

if(TARGET rodaq_ros2_reference_plugin)
    find_package(rclcpp REQUIRED)
    find_package(std_msgs REQUIRED)

    add_executable(
        plugin_reference_ros2_test
        reference/ros2_reference_test.cpp)
    target_include_directories(
        plugin_reference_ros2_test
        PRIVATE
            ${PROJECT_SOURCE_DIR}/plugin/ros2_acquisition/include)
    target_link_libraries(
        plugin_reference_ros2_test
        PRIVATE
            rodaq_plugin_test_support
            rodaq_plugin_runtime
            rclcpp::rclcpp
            std_msgs::std_msgs__rosidl_typesupport_cpp)
    target_compile_definitions(
        plugin_reference_ros2_test
        PRIVATE
            RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
    add_dependencies(plugin_reference_ros2_test rodaq_ros2_reference_plugin)
    rodaq_enable_project_warnings(plugin_reference_ros2_test)
    add_test(NAME plugin_reference_ros2_test COMMAND plugin_reference_ros2_test)

    if(TARGET rodaq_mcap_reference_plugin)
        add_executable(
            plugin_reference_integration_test
            reference/integration_test.cpp)
        target_include_directories(
            plugin_reference_integration_test
            PRIVATE
                ${PROJECT_SOURCE_DIR}/plugin/ros2_acquisition/include
                ${PROJECT_SOURCE_DIR}/plugin/mcap_storage/include)
        target_link_libraries(
            plugin_reference_integration_test
            PRIVATE
                rodaq_plugin_test_support
                rodaq_plugin_runtime
                rclcpp::rclcpp
                std_msgs::std_msgs__rosidl_typesupport_cpp
                mcap_vendor::mcap)
        target_compile_definitions(
            plugin_reference_integration_test
            PRIVATE
                RODAQ_TEST_PLUGIN_ROOT="${RODAQ_PLUGIN_PACKAGE_ROOT}")
        add_dependencies(
            plugin_reference_integration_test
            rodaq_ros2_reference_plugin
            rodaq_mcap_reference_plugin)
        rodaq_enable_project_warnings(plugin_reference_integration_test)
        add_test(
            NAME plugin_reference_integration_test
            COMMAND plugin_reference_integration_test)
    endif()

    add_executable(
        plugin_bounded_handoff_test
        reference/bounded_handoff_test.cpp)
    target_include_directories(
        plugin_bounded_handoff_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/reference)
    target_link_libraries(
        plugin_bounded_handoff_test
        PRIVATE
            rodaq_plugin_test_support)
    rodaq_enable_project_warnings(plugin_bounded_handoff_test)
    add_test(
        NAME plugin_bounded_handoff_test
        COMMAND plugin_bounded_handoff_test)
endif()
