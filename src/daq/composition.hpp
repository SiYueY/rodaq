#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/storage/types.hpp>
#include <rodaq/system/system_service.hpp>
#include <rodaq/task/task_service.hpp>

#include "../artifact/managed_volume.hpp"
#include "../artifact/volume_capacity.hpp"
#include "../pipeline/recording_instance.hpp"
#include "../task/task_service.hpp"

namespace rodaq::daq {

struct ResourceProfile {
    std::string name;
    pipeline::InstanceLimits instance;
    storage::PrepareConfig storage{};
    std::uint64_t segment_reservation_bytes{640ull * 1024 * 1024};
    artifact::ManifestLimits manifest;
    std::uint32_t max_prepared_descriptors{1024};
};

struct StorageAdmission {
    std::string plugin_id;
    std::string plugin_version;
    std::string format;
};

struct BackendQualification {
    std::string plugin_id;
    std::string plugin_version;
    BackendRole role{BackendRole::Acquisition};
    SupportStatus support_status{SupportStatus::Deferred};
    QualificationStatus qualification_status{QualificationStatus::NotQualified};
};

struct DeploymentSettings {
    std::uint64_t deployment_generation{1};
    std::string qualified_runtime_profile_id;
    std::string output_root;
    std::vector<ResourceProfile> profiles;
    std::vector<StorageAdmission> storage;
    std::vector<task::AllowedPlugin> allowed_plugins;
    std::vector<BackendQualification> backend_qualifications;
    task::TaskStorePolicy tasks;
    std::size_t max_tasks{4};
    std::uint32_t max_artifacts_per_volume{10000};
    std::chrono::nanoseconds native_timeout{std::chrono::seconds{5}};
    std::chrono::nanoseconds system_snapshot_max_age{std::chrono::seconds{2}};
};

struct CompositionOptions {
    std::filesystem::path task_root;
    std::shared_ptr<plugin::PluginManager> plugins;
    std::shared_ptr<artifact::ManagedVolume> volume;
    std::shared_ptr<artifact::VolumeCapacity> capacity;
    DeploymentSettings settings;
    pipeline::ReceiveClock sample{&time::sample_clocks};
    persistence::FileFaultHook task_file_faults;
};

struct StartupOptions;
struct DeploymentLease;

// Bounded Task, single-Volume composition. Owns fixed, separate control,
// create, prepare, cleanup, commit, and per-Pipeline Writer domains.
class Composition {
public:
    static Result<std::unique_ptr<Composition>> create(CompositionOptions options);
    static Result<std::unique_ptr<Composition>> open(StartupOptions options);
    Composition(const Composition&) = delete;
    Composition& operator=(const Composition&) = delete;
    ~Composition() noexcept;

    TaskService& tasks() noexcept;
    ArtifactService& artifacts() noexcept;
    SystemService& system() noexcept;
    Result<void> observe();
    Result<void> shutdown(std::chrono::steady_clock::time_point deadline);

private:
    struct State;
    explicit Composition(std::unique_ptr<State> state) noexcept;
    void retain_lease(std::shared_ptr<DeploymentLease> lease) noexcept;
    void publish_system_observation(
        const artifact::VolumeSpaceSnapshot& space, std::uint64_t total_bytes,
        bool force_protected) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::daq
