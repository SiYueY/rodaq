#pragma once

#include <rodaq/export.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rodaq/result.hpp>
#include <rodaq/task/runtime.hpp>
#include <rodaq/task/types.hpp>

namespace rodaq {
namespace daq {
class Composition;
}

namespace system {
struct SystemServiceOptions;
struct SystemObservation;
}  // namespace system

enum class VolumePressure { Normal, Low, Critical, Protected };
enum class BackendRole { Acquisition, Storage };
enum class SupportStatus { Required, QualifiedCandidate, Deferred };
enum class QualificationStatus { Qualified, NotQualified };

struct ProcessResourceSnapshot {
    double cpu_cores{0.0};
    std::uint64_t rss_bytes{0};
    std::uint64_t threads{0};
    std::uint64_t open_fds{0};
};

struct VolumeStatus {
    std::string name;
    std::uint64_t total_bytes{0};
    std::uint64_t free_bytes{0};
    VolumePressure pressure{VolumePressure::Protected};
};

struct SystemBacklogSnapshot {
    std::uint64_t commit_items{0};
    std::uint64_t commit_bytes{0};
};

// Immutable point-in-time projection of Core health, process resources,
// volume pressure and commit backlog. Queries never perform I/O.
struct SystemSnapshot {
    bool core_ready{false};
    ProcessResourceSnapshot resources;
    std::vector<VolumeStatus> volumes;
    SystemBacklogSnapshot backlog;
    WallTime captured_at;
    std::uint64_t snapshot_sequence{0};
    SnapshotFreshness freshness{SnapshotFreshness::NotObserved};
    std::chrono::nanoseconds age{0};
};

struct BackendCapability {
    std::string plugin_id;
    std::string plugin_version;
    // Absent when a plugin is installed on disk but not declared in deployment policy.
    std::optional<BackendRole> role;
    bool installed{false};
    bool allowed{false};
    SupportStatus support_status{SupportStatus::Deferred};
    QualificationStatus qualification_status{QualificationStatus::NotQualified};
};

struct ResourceProfileCapability {
    std::string name;
    std::uint64_t max_segment_physical_bytes{0};
    std::uint32_t max_streams_per_pipeline{64};
};

struct CoreLimitsCapability {
    std::uint32_t max_page_size{256};
    std::chrono::nanoseconds recommended_polling_floor{std::chrono::seconds{1}};
};

// Immutable deployment/plugin capability projection. Separates Installed,
// Allowed, SupportStatus and QualificationStatus without guessing.
struct SystemCapabilities {
    std::string application_version;
    std::vector<BackendCapability> backends;
    std::vector<std::string> output_roots;
    std::vector<ResourceProfileCapability> resource_profiles;
    CoreLimitsCapability limits;
};

// Read-only projection facade owned by DAQ composition. Does not hold a second
// authority over Task, Pipeline, Plugin or Volume state. Thread-safe.
class SystemService {
public:
    SystemService(const SystemService&) = delete;
    SystemService& operator=(const SystemService&) = delete;
    RODAQ_API ~SystemService() noexcept;

    RODAQ_API Result<SystemSnapshot> snapshot() const;
    RODAQ_API Result<SystemCapabilities> capabilities() const;

private:
    friend class daq::Composition;
    struct State;
    static Result<std::unique_ptr<SystemService>> create(system::SystemServiceOptions options);
    explicit SystemService(std::shared_ptr<State> state) noexcept;
    Result<void> refresh(const system::SystemObservation& observation);
    void mark_observation_degraded() noexcept;

    std::shared_ptr<State> state_;
};

}  // namespace rodaq
