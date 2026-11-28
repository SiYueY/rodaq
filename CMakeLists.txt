cmake_minimum_required(VERSION 3.22)

project(RoDAQ VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RODAQ_WARNINGS_AS_ERRORS "Treat project compiler warnings as errors" OFF)
include(CTest)
include(GNUInstallDirs)
include(cmake/RoDAQWarnings.cmake)
include(cmake/RoDAQQuality.cmake)
include(cmake/RoDAQPlugin.cmake)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_FUZZ OFF CACHE BOOL "" FORCE)
set(FMT_CUDA_TEST OFF CACHE BOOL "" FORCE)
set(FMT_OS ON CACHE BOOL "" FORCE)
set(FMT_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/fmt)

set(SPDLOG_BUILD_ALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_SYSTEM_INCLUDES ON CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS OFF CACHE BOOL "" FORCE)
set(SPDLOG_NO_THREAD_ID OFF CACHE BOOL "" FORCE)
set(SPDLOG_NO_ATOMIC_LEVELS OFF CACHE BOOL "" FORCE)
set(SPDLOG_NO_TZ_OFFSET OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/spdlog EXCLUDE_FROM_ALL)

# Plugin SDK dependencies stay independent of DAQ domain implementation.
find_package(nlohmann_json 3.10.5 EXACT REQUIRED CONFIG)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
add_library(rodaq_plugin SHARED
    src/logging.cpp
    src/plugin/catalog.cpp
    src/plugin/loader.cpp
    src/plugin/invocation.cpp
    src/plugin/lifecycle.cpp
    src/plugin/initialize.cpp
    src/plugin/close.cpp
    src/plugin/manager.cpp)
add_library(rodaq::plugin ALIAS rodaq_plugin)
set_target_properties(rodaq_plugin PROPERTIES EXPORT_NAME plugin)
target_compile_features(rodaq_plugin PUBLIC cxx_std_17)
target_include_directories(rodaq_plugin PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(rodaq_plugin SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/tl_expected/include>)
target_link_libraries(rodaq_plugin PUBLIC fmt::fmt
    PRIVATE spdlog::spdlog nlohmann_json::nlohmann_json Threads::Threads ${CMAKE_DL_LIBS})
rodaq_enable_project_warnings(rodaq_plugin)

add_library(rodaq_daq SHARED
    src/execution_lane.cpp
    src/uuid.cpp
    src/persistence/atomic_file.cpp
    src/persistence/digest.cpp
    src/ros2_subscription.cpp)
target_sources(rodaq_daq PRIVATE
    src/daq/composition.cpp src/daq/run_configuration.cpp src/daq/runtime.cpp src/daq/startup.cpp
    src/system/system_service.cpp
    src/task/config.cpp src/task/identity.cpp src/task/operation_history.cpp
    src/task/task_store.cpp src/task/task_service.cpp src/task/task_execution.cpp
    src/task/task_runtime.cpp
    src/time/clock.cpp
    src/time/wall_time.cpp)
target_sources(rodaq_daq PRIVATE
    src/pipeline/instance_id.cpp src/pipeline/admission_gate.cpp src/pipeline/payload_pool.cpp
    src/pipeline/descriptor.cpp
    src/pipeline/record_budget.cpp src/pipeline/record.cpp src/pipeline/record_queue.cpp
    src/pipeline/pipeline_controller.cpp src/pipeline/pipeline_rotation.cpp)
target_sources(rodaq_daq PRIVATE
    src/artifact/volume_capacity.cpp
    src/artifact/managed_volume.cpp
    src/artifact/sealed_files.cpp
    src/artifact/segment_committer.cpp
    src/artifact/artifact_catalog.cpp src/artifact/artifact_service.cpp
    src/artifact/artifact_query.cpp src/artifact/artifact_reader.cpp
    src/artifact/startup_scan.cpp
    src/artifact/artifact_delete.cpp
    src/artifact/artifact_retention.cpp
    src/artifact/manifest.cpp
    src/pipeline/segment_metadata.cpp
    src/pipeline/recording_segment.cpp
    src/pipeline/admission.cpp
    src/pipeline/pipeline_writer.cpp
    src/pipeline/stream_table.cpp
    src/pipeline/recording_instance.cpp)
add_library(rodaq::daq ALIAS rodaq_daq)
set_target_properties(rodaq_daq PROPERTIES EXPORT_NAME daq)
target_compile_definitions(rodaq_daq PRIVATE RODAQ_APPLICATION_VERSION="${PROJECT_VERSION}")
target_link_libraries(rodaq_daq PUBLIC rodaq::plugin
    PRIVATE nlohmann_json::nlohmann_json OpenSSL::Crypto)
rodaq_enable_project_warnings(rodaq_daq)

option(RODAQ_BUILD_BEHAVIORAL_FAKES "Build qualification-only fault-injection backends" OFF)
option(RODAQ_BUILD_MCAP_STORAGE "Build the MCAP format backend using prepared ROS dependencies" OFF)
option(RODAQ_BUILD_ROS2_ACQUISITION "Build ROS 2 acquisition using prepared ROS dependencies" OFF)
option(RODAQ_BUILD_SERVER "Build the RoDAQ Drogon 1.9.14 HTTP server" OFF)
set(RODAQ_CONFIG_EXTRA_DEPS "")
if(RODAQ_BUILD_ROS2_ACQUISITION)
    set(RODAQ_CONFIG_EXTRA_DEPS "find_dependency(ament_index_cpp)")
    add_subdirectory(plugins/support/ros2)
    add_subdirectory(plugins/acquisition/ros2)
endif()
if(RODAQ_BUILD_MCAP_STORAGE)
    add_subdirectory(plugins/storage/mcap)
endif()
if(RODAQ_BUILD_BEHAVIORAL_FAKES)
    add_subdirectory(plugins/fake_storage)
    add_subdirectory(plugins/fake_acquisition)
endif()
if(RODAQ_BUILD_SERVER)
    add_subdirectory(server)
endif()

include(CMakePackageConfigHelpers)
install(TARGETS rodaq_plugin rodaq_daq EXPORT RoDAQTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/rodaq DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES schemas/task_config.schema.json DESTINATION ${CMAKE_INSTALL_DATADIR}/rodaq/schemas)
install(DIRECTORY third_party/tl_expected/include/tl DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES third_party/tl_expected/COPYING
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/RoDAQ RENAME tl_expected-LICENSE)
install(FILES third_party/spdlog/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/RoDAQ RENAME spdlog-LICENSE)
configure_package_config_file(cmake/RoDAQConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/RoDAQConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RoDAQ)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/RoDAQConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(EXPORT RoDAQTargets NAMESPACE rodaq:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RoDAQ)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/RoDAQConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/RoDAQConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RoDAQ)
install(FILES cmake/RoDAQPlugin.cmake cmake/plugin_metadata.py cmake/plugin.exports
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RoDAQ)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
