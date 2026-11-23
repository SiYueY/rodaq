#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/task/config.hpp>

#include "ledger_store.hpp"
#include "manifest.hpp"
#include "volume_capacity.hpp"
#include "../pipeline/sealed_segment.hpp"

namespace rodaq::artifact {

enum class ReadStep { Opening, Reading };
enum class DeleteReason { Retention, Reset, ManualRecovery };
enum class DeleteStep { UnlinkFile, SyncDirectory, UnlinkManifest, RemoveDirectory, SyncParent };

struct CatalogOptions {
    std::shared_ptr<ManagedVolume> volume;
    std::shared_ptr<LedgerStore> ledger;
    std::shared_ptr<VolumeCapacity> capacity;
    ManifestLimits manifest;
    std::uint32_t max_artifacts{4096};
    std::uint32_t max_read_leases{64};
    std::size_t max_read_bytes{1024 * 1024};
    std::chrono::nanoseconds read_lease_duration{std::chrono::seconds{30}};
    // Private deterministic fault seam, executed outside the index lock.
    std::function<int(ReadStep)> read_faults;
    std::function<int(DeleteStep)> delete_faults;
};

struct CatalogSnapshot {
    StatusCode protection{StatusCode::Ok};
    std::uint32_t visible{0};
    std::uint32_t pending{0};
    std::uint32_t commit_inflight{0};
    std::uint32_t delete_inflight{0};
    std::uint32_t collect_inflight{0};
    std::uint32_t read_leases{0};
    std::uint32_t reads_inflight{0};
};

struct CatalogState;

struct RetentionSelection {
    std::optional<ArtifactId> candidate;
    std::uint64_t committed_recording_end_ns{0};
    std::uint64_t retained_physical_bytes{0};
    bool byte_target_satisfied{true};
};

// Rebuildable index and retained actual handoff/read owners, not a second
// durable authority. Ledger/Committer own publication; Catalog exposes only
// verified committed evidence and never releases disk charges by eviction.
class ArtifactCatalog {
public:
    static Result<std::shared_ptr<ArtifactCatalog>> create(CatalogOptions options);
    Result<void> commit(const pipeline::SealedPipelineSegment& sealed);
    // Startup work only. Capacity starts from independently verified recovery
    // totals; each committed row claims its retained charge exactly once.
    Result<void> rebuild();
    Result<ArtifactPage> query(const ArtifactQuery& query) const;
    Result<ArtifactInfo> get(ArtifactId id) const;
    Result<ArtifactReadHandle> acquire(ArtifactId id, ArtifactFileId file);
    Result<void> block_reads(ArtifactId id);
    void cancel_readers(std::optional<ArtifactId> id = {});
    Result<void> wait_readers(ArtifactId id, std::chrono::steady_clock::time_point deadline);
    // Maintenance actual execution only. Caller owns policy/generation selection;
    // the Ledger owns durable intent, and the retained entry owns disk charges.
    // Deadline bounds lease observation, never an already-entered syscall.
    Result<void> delete_artifact(
        ArtifactId id, std::uint64_t generation, DeleteReason reason, WallTime now,
        std::chrono::steady_clock::time_point deadline);
    // Task resolves historical immutable configuration modes into this bounded
    // version set. The time floor comes only from verified committed checkpoints,
    // never from Task Open time. Revalidate current policy before delete intent.
    Result<RetentionSelection> select_retention(
        TaskId task, const RetentionPolicy& policy,
        const std::vector<std::string>& eligible_config_versions,
        std::uint64_t committed_recording_floor_ns = 0) const;
    Result<std::vector<std::string>> retention_versions(TaskId task) const;
    // One eligible live tombstone per maintenance pass. The Task supplies a
    // durable committed checkpoint before its source artifact proof is removed.
    Result<bool> collect_deleted(
        TaskId task, std::uint64_t committed_recording_floor_ns, WallTime now,
        std::chrono::nanoseconds audit_window);
    CatalogSnapshot snapshot() const;

private:
    explicit ArtifactCatalog(std::shared_ptr<CatalogState> state) noexcept;
    std::shared_ptr<CatalogState> state_;
};

}  // namespace rodaq::artifact
