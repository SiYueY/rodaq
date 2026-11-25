#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/storage/types.hpp>
#include <rodaq/system/system_service.hpp>
#include <rodaq/task/task_service.hpp>

#include "../artifact/ledger_store.hpp"
#include "../artifact/managed_volume.hpp"
#include "../artifact/recovery_scan.hpp"
#include "../artifact/segment_id_store.hpp"
#include "../artifact/volume_capacity.hpp"
#include "../pipeline/recording_instance.hpp"
#include "../task/task_service.hpp"
#include "../time/clock_correlation.hpp"

namespace rodaq::daq {

struct ResourceProfile {
    std::string name;
    pipeline::InstanceLimits instance;
    storage::PrepareConfig storage{};
    artifact::ManifestLimits manifest;
    std::uint32_t max_prepared_descriptors{1024};
    std::uint32_t sequence_reservation{65536};
    std::uint32_t descriptor_reservation{256};
};

struct StorageAdmission {
    std::string plugin_id;
    std::string plugin_version;
    std::string format;
};

struct DeploymentSettings {
    std::uint64_t deployment_generation{0};
    // Resolved by the Core release/support-matrix owner, never by a Plugin.
    std::string qualified_runtime_profile_id;
    std::string output_root;
    std::vector<ResourceProfile> profiles;
    std::vector<StorageAdmission> storage;
    std::vector<task::AllowedPlugin> allowed_plugins;
    task::TaskStorePolicy tasks;
    // Registered Task capacity: 1..16; total preallocated Writer lanes <=256.
    // Each Task owns a full max_pipelines slice, including while it is idle.
    std::size_t max_tasks{1};
    std::size_t create_recovery_slots{8};
    std::chrono::nanoseconds native_timeout{std::chrono::seconds{5}};
    std::chrono::nanoseconds artifact_audit_window{std::chrono::hours{24}};
    task::RuntimeRecoveryPolicy runtime_recovery;
};

struct CompositionOptions {
    // Startup has exclusively opened/reconciled these actual authorities. This
    // composition never guesses absence, initializes a missing authority, or
    // adopts unregistered output. Full boot reconciliation belongs to S4C.
    std::filesystem::path task_root;
    std::shared_ptr<plugin::PluginManager> plugins;
    std::shared_ptr<time::ClockCorrelation> clock;
    time::BootIdentity boot;
    std::shared_ptr<artifact::ManagedVolume> volume;
    std::shared_ptr<artifact::VolumeCapacity> capacity;
    std::shared_ptr<artifact::LedgerStore> ledger;
    std::shared_ptr<artifact::SegmentIdStore> segment_ids;
    std::optional<artifact::VolumeRecoveryReport> recovery_report;
    DeploymentSettings settings;
    pipeline::ReceiveClock sample{&time::sample_clocks};
    // Private persistence fault seam; actual calls remain outside Task control.
    std::function<int(task::ServiceFileDomain, persistence::FileStep)> task_file_faults;
};

struct StartupOptions;
struct DeploymentLease;

// Bounded Task, single-Volume composition. Owns fixed, separate control,
// create, prepare, cleanup and per-Pipeline Writer domains. No implicit monitor
// thread: the daemon explicitly schedules observe() on maintenance capacity.
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
    // Caller retains the composition through actual observation return, even
    // when Core shutdown completes independently of a blocked filesystem call.
    Result<void> observe();
    // Deadline is an observer bound. A failure retains every actual owner;
    // retry on this same composition or use external process supervision.
    // Shutdown never manufactures a user Stop or advances Control generation.
    Result<void> shutdown(std::chrono::steady_clock::time_point deadline);

private:
    struct State;
    explicit Composition(std::unique_ptr<State> state) noexcept;
    void retain_lease(std::shared_ptr<DeploymentLease> lease) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::daq
