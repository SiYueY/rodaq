add_executable(rodaq_payload_pool_benchmark payload_pool_benchmark.cpp)
target_link_libraries(rodaq_payload_pool_benchmark
    PRIVATE
        benchmark::benchmark
        rodaq_runtime_testing
)
rodaq_enable_warnings(rodaq_payload_pool_benchmark)
rodaq_enable_sanitizers(rodaq_payload_pool_benchmark)

add_executable(rodaq_runtime_pipeline_benchmark runtime_pipeline_benchmark.cpp)
target_link_libraries(rodaq_runtime_pipeline_benchmark
    PRIVATE
        benchmark::benchmark
        rodaq_runtime_testing
)
rodaq_enable_warnings(rodaq_runtime_pipeline_benchmark)
rodaq_enable_sanitizers(rodaq_runtime_pipeline_benchmark)

add_executable(rodaq_joint_pipeline_benchmark joint_pipeline_benchmark.cpp)
target_link_libraries(rodaq_joint_pipeline_benchmark
    PRIVATE
        benchmark::benchmark
        rodaq_runtime_testing
)
rodaq_enable_warnings(rodaq_joint_pipeline_benchmark)
rodaq_enable_sanitizers(rodaq_joint_pipeline_benchmark)

function(rodaq_add_capacity_benchmark target source)
    add_executable("${target}" "${source}")
    target_link_libraries("${target}"
        PRIVATE
            benchmark::benchmark
            rodaq_runtime_testing
    )
    target_include_directories("${target}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    rodaq_enable_warnings("${target}")
    rodaq_enable_sanitizers("${target}")
endfunction()

rodaq_add_capacity_benchmark(
    rodaq_camera_pipeline_benchmark
    camera_pipeline_benchmark.cpp
)
rodaq_add_capacity_benchmark(
    rodaq_mixed_workload_benchmark
    mixed_workload_benchmark.cpp
)
rodaq_add_capacity_benchmark(
    rodaq_payload_class_benchmark
    payload_class_benchmark.cpp
)
rodaq_add_capacity_benchmark(
    rodaq_storage_slot_benchmark
    storage_slot_benchmark.cpp
)
rodaq_add_capacity_benchmark(
    rodaq_release_worker_benchmark
    release_worker_benchmark.cpp
)
