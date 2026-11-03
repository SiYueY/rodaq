#ifndef RODAQ_RUNTIME_RUNTIME_HPP_
#define RODAQ_RUNTIME_RUNTIME_HPP_

#include "plugin/config_view.hpp"

#include <rodaq/plugin/abi/runtime.h>
#include <rodaq/result.hpp>
#include <rodaq/runtime/admission.hpp>
#include <rodaq/runtime/runtime_stats.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rodaq::runtime {

enum class RuntimeState {
    Constructed,
    Configured,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

struct RuntimePluginOptions {
    std::filesystem::path path;
    plugin::ConfigTree config;
};

struct RuntimeOptions {
    std::size_t max_sources = 64;
    std::size_t max_clocks = 64;
    std::size_t max_schemas = 64;
    std::size_t max_timestamp_key_sets = 64;
    std::size_t max_streams = 64;
    std::size_t max_stream_descriptors = 128;
    std::size_t max_bindings = 128;
    std::size_t structural_arena_bytes = 1024U * 1024U;

    std::size_t payload_block_bytes = 4U * 1024U * 1024U;
    std::size_t payload_block_count = 64;
    std::size_t max_single_payload_bytes = 4U * 1024U * 1024U;

    std::size_t dispatch_capacity = 64;
    std::size_t operation_capacity = 64;
    std::size_t completion_capacity = 64;

    std::chrono::nanoseconds submit_wait_timeout = std::chrono::seconds(1);
    std::chrono::nanoseconds stop_progress_timeout = std::chrono::seconds(2);
    AdmissionMode admission_mode = AdmissionMode::Wait;

    std::string session_id = "rodaq";
    std::string target_uri;

    RuntimePluginOptions storage;
    std::vector<RuntimePluginOptions> acquisitions;
};

class Runtime final {
public:
    explicit Runtime(RuntimeOptions options);
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
    ~Runtime();

    Result<void> Configure();
    Result<void> Start();
    Result<void> Stop();

    RuntimeState State() const noexcept;
    Result<rodaq_runtime_lifecycle_snapshot> LifecycleSnapshot() const;
    Result<Error> PrimaryFailure() const;
    Result<std::vector<Error>> SecondaryFailures() const;
    std::uint64_t SecondaryFailureOverflowCount() const noexcept;
    Result<RuntimeStatsSnapshot> Stats() const;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq::runtime

#endif  // RODAQ_RUNTIME_RUNTIME_HPP_
