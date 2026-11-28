#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/task/config.hpp>

#include "manifest.hpp"
#include "sealed_segment.hpp"
#include "volume_capacity.hpp"

namespace rodaq::artifact {

enum class ReadStep { Opening, Reading };
enum class DeleteStep { RenameToDeleting, RemoveTree, SyncParent };

struct CatalogOptions {
    std::shared_ptr<ManagedVolume> volume;
    std::shared_ptr<VolumeCapacity> capacity;
    ManifestLimits manifest;
    std::uint32_t max_artifacts{10000};
    std::uint32_t max_read_leases{64};
    std::size_t max_read_bytes{1024 * 1024};
    std::chrono::nanoseconds read_lease_duration{std::chrono::seconds{30}};
    DurabilityProfile durability{DurabilityProfile::Normal};
    // Private deterministic fault seams, executed outside the index lock.
    std::function<int(ReadStep)> read_faults;
    std::function<int(DeleteStep)> delete_faults;
};

struct CatalogSnapshot {
    StatusCode protection{StatusCode::Ok};
    std::uint32_t visible{0};
    std::uint32_t commit_inflight{0};
    std::uint32_t delete_inflight{0};
    std::uint32_t read_leases{0};
    std::uint32_t reads_inflight{0};
    std::uint32_t corrupt_artifacts{0};
};

struct CatalogState;
struct StartupScanReport;

struct RetentionSelection {
    std::optional<ArtifactId> candidate;
    std::uint64_t committed_recording_end_ns{0};
    std::uint64_t retained_physical_bytes{0};
    bool byte_target_satisfied{true};
};

// In-memory index rebuilt synchronously from final Artifact directories +
// manifest.json on startup. No second durable Ledger authority or quarantine.
class ArtifactCatalog {
public:
    static Result<std::shared_ptr<ArtifactCatalog>> create(CatalogOptions options);
    // Synchronous startup scan before Server enters Ready.
    Result<StartupScanReport> startup_scan();
    // Publishes a sealed Pipeline segment into a final Artifact and Catalog.
    Result<void> commit(SealedSegment sealed, bool* settled = nullptr);
    // Caller still owns a sealed handoff rejected before queue admission.
    Result<void> abort(SealedSegment& sealed);
    Result<ArtifactPage> query(const ArtifactQuery& query) const;
    Result<ArtifactInfo> get(ArtifactId id) const;
    Result<ArtifactReadHandle> acquire(ArtifactId id, ArtifactFileId file);
    Result<void> block_reads(ArtifactId id);
    void cancel_readers(std::optional<ArtifactId> id = {}) noexcept;
    Result<void> wait_readers(ArtifactId id, std::chrono::steady_clock::time_point deadline);
    // Rename-then-delete execution: blocks new reads, waits for active leases,
    // atomically renames <id> -> <id>.deleting, removes from Catalog, and runs
    // bounded physical tree deletion.
    Result<void> delete_artifact(ArtifactId id, std::chrono::steady_clock::time_point deadline);
    // Read-only artifact-side predicate for Task retirement.
    Result<bool> retire_eligible(TaskId task) const;
    // Snapshot of currently visible final Artifact IDs belonging to task.
    Result<std::vector<ArtifactId>> reset_targets(TaskId task) const;
    // Selects the oldest eligible artifact violating the Task's current policy.
    Result<RetentionSelection> select_retention(
        TaskId task, const RetentionPolicy& policy,
        std::uint64_t committed_recording_floor_ns = 0) const;
    CatalogSnapshot snapshot() const;

private:
    explicit ArtifactCatalog(std::shared_ptr<CatalogState> state) noexcept;
    std::shared_ptr<CatalogState> state_;
};

}  // namespace rodaq::artifact
