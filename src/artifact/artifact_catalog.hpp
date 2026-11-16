#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <rodaq/artifact/artifact_service.hpp>

#include "ledger_store.hpp"
#include "manifest.hpp"
#include "volume_capacity.hpp"
#include "../pipeline/sealed_segment.hpp"

namespace rodaq::artifact {

enum class ReadStep { Opening, Reading };

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
};

struct CatalogSnapshot {
    StatusCode protection{StatusCode::Ok};
    std::uint32_t visible{0};
    std::uint32_t pending{0};
    std::uint32_t commit_inflight{0};
    std::uint32_t read_leases{0};
    std::uint32_t reads_inflight{0};
};

struct CatalogState;

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
    CatalogSnapshot snapshot() const;

private:
    explicit ArtifactCatalog(std::shared_ptr<CatalogState> state) noexcept;
    std::shared_ptr<CatalogState> state_;
};

}  // namespace rodaq::artifact
