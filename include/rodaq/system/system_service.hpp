#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rodaq/result.hpp>
#include <rodaq/task/runtime.hpp>
#include <rodaq/task/types.hpp>

namespace rodaq {

enum class VolumePressure { Normal, Low, Critical, Protected };
enum class BackendRole { Acquisition, Storage };
enum class BackendSupportStatus { Required, QualifiedCandidate, Deferred };
enum class BackendQualificationStatus { Qualified, NotQualified };

struct ProcessResources {
    std::uint32_t cpu_cores{0};
    std::uint64_t rss_bytes{0};
    std::uint64_t threads{0};
    std::uint64_t open_fds{0};
};

struct VolumeStatus {
    std::string name;
    std::uint64_t total_bytes{0};
    std::uint64_t free_bytes{0};
    std::uint64_t managed_bytes{0};
    std::uint64_t quarantined_bytes{0};
    VolumePressure pressure{VolumePressure::Protected};
    StatusCode protection{StatusCode::Ok};
};

struct BacklogStatus {
    std::uint64_t commit_items{0};
    std::uint64_t commit_bytes{0};
    std::uint64_t active_downloads{0};
    std::uint32_t quarantined_items{0};
    std::uint32_t verification_required_items{0};
    std::uint32_t corrupted_committed_items{0};
};

struct SystemRecoveryStatus {
    bool complete{true};
    std::optional<std::string> reason;
    std::optional<double> progress;
};

struct SystemSnapshot {
    bool core_ready{false};
    SystemRecoveryStatus recovery;
    ProcessResources resources;
    std::vector<VolumeStatus> volumes;
    BacklogStatus backlog;
    WallTime captured_at;
    std::uint64_t snapshot_sequence{0};
    SnapshotFreshness freshness{SnapshotFreshness::NotObserved};
    std::chrono::nanoseconds age{0};
};

struct BackendCapability {
    std::string plugin_id;
    std::string plugin_version;
    BackendRole role{BackendRole::Acquisition};
    bool installed{false};
    bool allowed{false};
    BackendSupportStatus support_status{BackendSupportStatus::Deferred};
    BackendQualificationStatus qualification_status{BackendQualificationStatus::NotQualified};
};

struct ApiLimits {
    std::uint64_t max_request_body_bytes{1024 * 1024};
    std::uint32_t max_page_size{256};
    std::chrono::nanoseconds idempotency_retention{std::chrono::hours{24}};
    std::chrono::nanoseconds recommended_polling_floor{std::chrono::milliseconds{500}};
};

struct SystemCapabilities {
    std::string application_version;
    std::vector<BackendCapability> backends;
    std::vector<std::string> output_roots;
    std::vector<std::string> resource_profiles;
    ApiLimits api_limits;
};

// Read-only system projection facade. Aggregates facts already published by
// their authoritative owners without creating a second state authority.
class SystemService {
public:
    using SnapshotProvider = std::function<Result<SystemSnapshot>()>;
    using CapabilitiesProvider = std::function<Result<SystemCapabilities>()>;

    static Result<std::unique_ptr<SystemService>> create(
        SnapshotProvider snapshot_provider, CapabilitiesProvider capabilities_provider);

    SystemService(const SystemService&) = delete;
    SystemService& operator=(const SystemService&) = delete;
    ~SystemService() noexcept;

    Result<SystemSnapshot> snapshot() const;
    Result<SystemCapabilities> capabilities() const;

private:
    struct State;
    explicit SystemService(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq
