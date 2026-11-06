#ifndef RODAQ_DAQ_CONFIG_HPP_
#define RODAQ_DAQ_CONFIG_HPP_

#include <rodaq/config/config.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rodaq {

enum class AdmissionMode {
    Reject,
    Wait,
};

enum class LogLevel : std::uint8_t {
    Trace = 1,
    Debug = 2,
    Info = 3,
    Warn = 4,
    Error = 5,
    Critical = 6,
    Off = 7,
};

struct LoggingConfig {
    LogLevel level = LogLevel::Info;
    std::size_t queue_records = 8192;
    std::size_t queue_bytes = 8U * 1024U * 1024U;
    std::chrono::nanoseconds flush_timeout = std::chrono::seconds(2);
    std::chrono::nanoseconds shutdown_timeout = std::chrono::seconds(3);
    bool stderr_sink = true;
};

struct PayloadClassConfig {
    std::size_t max_payload_bytes = 0;
    std::size_t block_count = 0;
};

struct PayloadPoolConfig {
    std::vector<PayloadClassConfig> classes;
};

struct ResourceConfig {
    ResourceConfig() { payload_pool.classes.push_back(PayloadClassConfig{4U * 1024U * 1024U, 64}); }

    std::size_t max_sources = 64;
    std::size_t max_clocks = 64;
    std::size_t max_schemas = 64;
    std::size_t max_timestamp_key_sets = 64;
    std::size_t max_streams = 64;
    std::size_t max_stream_descriptors = 128;
    std::size_t max_bindings = 128;
    std::size_t structural_arena_bytes = 1024U * 1024U;

    PayloadPoolConfig payload_pool;
    std::size_t max_single_payload_bytes = 4U * 1024U * 1024U;

    std::size_t dispatch_capacity = 64;
    std::size_t operation_capacity = 64;
    std::size_t completion_capacity = 64;
    std::size_t payload_release_workers = 1;
};

struct AcquisitionPluginConfig {
    std::filesystem::path path;
    ConfigTree config;
};

struct StoragePluginConfig {
    std::filesystem::path path;
    ConfigTree config;
};

struct DAQConfig {
    std::string acquisition_id = "rodaq";
    std::string target_uri;

    ResourceConfig resources;
    std::chrono::nanoseconds submit_wait_timeout = std::chrono::seconds(1);
    std::chrono::nanoseconds stop_progress_timeout = std::chrono::seconds(2);
    AdmissionMode admission_mode = AdmissionMode::Wait;
    LoggingConfig logging;

    StoragePluginConfig storage;
    std::vector<AcquisitionPluginConfig> acquisitions;
};

}  // namespace rodaq

#endif  // RODAQ_DAQ_CONFIG_HPP_
