#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "artifact_catalog.hpp"
#include "segment_committer.hpp"
#include "../persistence/file_descriptor.hpp"

namespace rodaq::artifact {

struct CatalogFile {
    std::string path;
    struct stat identity {};
};

struct CatalogEntry {
    ArtifactInfo info;
    std::string config_version;
    std::vector<CatalogFile> files;
    struct stat directory {};
    struct stat manifest {};
    std::optional<ArtifactCharge> charge;
    bool read_blocked{false};
    std::uint32_t readers{0};
};

struct CommitFrame {
    explicit CommitFrame(pipeline::SealedPipelineSegment value) : sealed(std::move(value)) {}

    pipeline::SealedPipelineSegment sealed;
    std::unique_ptr<SealedFiles> files;
    std::optional<CommitReservation> reservation;
    std::unique_ptr<SegmentCommitter> committer;
};

enum class CatalogWork { None, Commit, Delete };

struct CatalogSlot {
    std::uint64_t segment_id{0};
    std::uint64_t work_id{0};
    std::shared_ptr<CatalogEntry> entry;
    std::shared_ptr<CommitFrame> frame;
    CatalogWork work{CatalogWork::None};
    bool visible{false};
    StatusCode failure{StatusCode::Ok};
};

struct ReadSlot {
    bool busy{false};
    std::uint64_t generation{0};
    std::weak_ptr<ReadBacking> reader;
};

struct CatalogState {
    CatalogOptions options;
    mutable std::mutex index;
    std::condition_variable reader_changed;
    std::vector<CatalogSlot> slots;
    std::vector<ReadSlot> readers;
    bool accepting_reads{true};
    bool rebuilding{false};
    std::uint32_t read_leases{0};
    std::atomic<std::uint32_t> active_reads{0};

    std::size_t find(std::uint64_t id) const noexcept;
    Result<bool> public_entry(const CatalogSlot& slot) const;
    void corrupt(std::uint64_t id) noexcept;
};

struct ReadBacking {
    std::shared_ptr<CatalogState> owner;
    std::shared_ptr<CatalogEntry> entry;
    std::optional<persistence::FileDescriptor> file;
    struct stat identity {};
    std::size_t slot{0};
    std::uint64_t generation{0};
    std::uint64_t length{0};
    bool registered{false};
    std::chrono::steady_clock::time_point deadline;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> active{false};
    ~ReadBacking() noexcept;
};

bool same_regular_file(const struct stat& expected, const struct stat& actual) noexcept;
bool same_segment_directory(const struct stat& expected, const struct stat& actual) noexcept;
Result<persistence::FileDescriptor> open_final_directory(
    const std::shared_ptr<ManagedVolume>& volume, std::uint64_t segment);
Result<std::shared_ptr<CatalogEntry>> verify_committed(
    const CatalogOptions& options, const LedgerRecord& record);

}  // namespace rodaq::artifact
