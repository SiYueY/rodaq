add_executable(rodaq_payload_pool_benchmark payload_pool_benchmark.cpp)
target_link_libraries(rodaq_payload_pool_benchmark
    PRIVATE
        benchmark::benchmark
        rodaq_runtime
)
rodaq_enable_warnings(rodaq_payload_pool_benchmark)
rodaq_enable_sanitizers(rodaq_payload_pool_benchmark)

add_executable(rodaq_runtime_pipeline_benchmark runtime_pipeline_benchmark.cpp)
target_link_libraries(rodaq_runtime_pipeline_benchmark
    PRIVATE
        benchmark::benchmark
        rodaq_runtime
)
rodaq_enable_warnings(rodaq_runtime_pipeline_benchmark)
rodaq_enable_sanitizers(rodaq_runtime_pipeline_benchmark)
