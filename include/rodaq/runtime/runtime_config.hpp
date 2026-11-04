#ifndef RODAQ_RUNTIME_RUNTIME_CONFIG_HPP_
#define RODAQ_RUNTIME_RUNTIME_CONFIG_HPP_

#include <rodaq/config/config.hpp>
#include <rodaq/runtime/admission.hpp>
#include <rodaq/runtime/logging.hpp>
#include <rodaq/runtime/payload_pool.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace rodaq::runtime {

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

struct PluginConfig {
    std::filesystem::path path;
    ConfigTree config;
};

struct StorageConfig {
    std::filesystem::path path;
    ConfigTree config;
};

struct RuntimeConfig {
    std::string session_id = "rodaq";
    std::string target_uri;

    ResourceConfig resources;
    std::chrono::nanoseconds submit_wait_timeout = std::chrono::seconds(1);
    std::chrono::nanoseconds stop_progress_timeout = std::chrono::seconds(2);
    AdmissionMode admission_mode = AdmissionMode::Wait;
    LoggingConfig logging;

    StorageConfig storage;
    std::vector<PluginConfig> acquisitions;
};

}  // namespace rodaq::runtime

#endif  // RODAQ_RUNTIME_RUNTIME_CONFIG_HPP_
