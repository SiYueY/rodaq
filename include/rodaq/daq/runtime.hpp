#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/result.hpp>
#include <rodaq/system/system_service.hpp>
#include <rodaq/task/task_service.hpp>

namespace rodaq {

enum class DaqOpenMode { Initialize, Existing };

struct AllowedBackend {
    std::string plugin_id;
    std::string plugin_version;
    BackendRole role{BackendRole::Acquisition};
    // Required when role == Storage (e.g. "mcap", "fake").
    std::string storage_format;
    SupportStatus support_status{SupportStatus::Deferred};
    QualificationStatus qualification_status{QualificationStatus::NotQualified};
};

struct DaqResourceProfile {
    std::string name;
    std::uint64_t max_records{4096};
    std::uint64_t max_logical_bytes{64ull * 1024 * 1024};
    std::uint64_t payload_pool_bytes{64ull * 1024 * 1024};
    std::uint64_t max_payload_bytes{16ull * 1024 * 1024};
    std::uint32_t payload_min_chunk_bytes{4096};
    std::uint32_t payload_max_chunks{4096};
    std::uint32_t queue_capacity{1024};
    std::uint32_t callback_concurrency{4};
    std::uint64_t max_segment_physical_bytes{512ull * 1024 * 1024};
    std::uint64_t max_segment_records{1000000};
    std::uint64_t max_segment_input_bytes{512ull * 1024 * 1024};
    std::uint32_t max_batch_records{256};
    std::uint64_t max_batch_input_bytes{16ull * 1024 * 1024};
    std::uint32_t max_streams{128};
    std::uint32_t max_descriptors{1024};
    std::uint32_t max_sealed_files{8};
    std::uint64_t max_manifest_bytes{1024 * 1024};
    std::uint32_t max_prepared_descriptors{1024};
    std::uint32_t sequence_reservation{65536};
    std::uint32_t descriptor_reservation{256};
};

struct DaqVolumePolicy {
    std::string name{"local"};
    std::uint64_t max_managed_bytes{64ull * 1024 * 1024 * 1024};
    std::uint64_t max_commit_bytes{64ull * 1024 * 1024};
    std::uint64_t stop_free_bytes{64ull * 1024 * 1024};
    std::uint64_t low_free_bytes{128ull * 1024 * 1024};
    std::uint64_t recovery_free_bytes{256ull * 1024 * 1024};
    std::chrono::nanoseconds space_sample_max_age{std::chrono::seconds{2}};
    std::chrono::nanoseconds recovery_stable_window{std::chrono::seconds{1}};
    std::uint32_t max_segments{64};
    std::uint32_t max_commit_slots{16};
    std::uint32_t max_artifact_slots{4096};
    std::uint32_t ledger_records{4096};
};

struct DaqOpenOptions {
    std::filesystem::path state_root;
    std::filesystem::path volume_root;
    DaqOpenMode mode{DaqOpenMode::Existing};
    std::vector<std::filesystem::path> plugin_search_paths;
    std::uint32_t max_loaded_plugins{32};
    std::uint32_t max_plugin_instances{64};
    std::size_t max_plugin_config_bytes{65536};
    std::chrono::nanoseconds plugin_shutdown_timeout{std::chrono::seconds{5}};
    std::uint64_t deployment_generation{1};
    std::string qualified_runtime_profile_id;
    std::size_t max_tasks{4};
    std::size_t max_pipelines_per_task{8};
    std::size_t create_recovery_slots{8};
    std::chrono::nanoseconds native_timeout{std::chrono::seconds{5}};
    std::chrono::nanoseconds artifact_audit_window{std::chrono::hours{24}};
    std::chrono::nanoseconds system_snapshot_max_age{std::chrono::seconds{2}};
    DaqVolumePolicy volume;
    std::vector<DaqResourceProfile> profiles;
    std::vector<AllowedBackend> allowed_backends;
};

// Public process-level DAQ service boundary for Server and C++ consumers.
// Exposes only formal TaskService, ArtifactService and SystemService facades.
// Caller must invoke shutdown() before destroying a live runtime.
class DaqRuntime {
public:
    static Result<std::unique_ptr<DaqRuntime>> open(DaqOpenOptions options);

    DaqRuntime(const DaqRuntime&) = delete;
    DaqRuntime& operator=(const DaqRuntime&) = delete;
    ~DaqRuntime() noexcept;

    TaskService& tasks() noexcept;
    ArtifactService& artifacts() noexcept;
    SystemService& system() noexcept;

    // Periodic maintenance tick (clock/volume sampling, retention, GC, system
    // projection refresh). Call from a bounded maintenance thread, never from
    // an HTTP event loop.
    Result<void> observe();

    // Deadline bounds observer wait; failure retains live owners so the caller
    // can retry on this runtime. Never manufactures a user Stop operation.
    Result<void> shutdown(std::chrono::steady_clock::time_point deadline);

private:
    struct State;
    explicit DaqRuntime(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

}  // namespace rodaq
