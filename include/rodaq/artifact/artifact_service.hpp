#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/task/types.hpp>

namespace rodaq {
namespace artifact {
class ArtifactCatalog;
struct ReadBacking;
}  // namespace artifact

enum class DurabilityProfile { Normal, PowerLossHardened };

struct ArtifactId {
    std::uint64_t value{0};
    friend bool operator==(ArtifactId a, ArtifactId b) noexcept { return a.value == b.value; }
};

// Scoped to its Artifact's immutable, bytewise-sorted file set.
struct ArtifactFileId {
    std::uint32_t value{0};
    friend bool operator==(ArtifactFileId a, ArtifactFileId b) noexcept {
        return a.value == b.value;
    }
};

Result<ArtifactId> parse_artifact_id(std::string_view text);
Result<ArtifactFileId> parse_artifact_file_id(std::string_view text);
std::string to_string(ArtifactId id);
std::string to_string(ArtifactFileId id);

struct RecordingRange {
    std::uint64_t begin_ns{0};
    std::uint64_t end_ns{0};
};

struct ArtifactWallRange {
    WallTime begin;
    WallTime end;
};

struct ArtifactSummary {
    ArtifactId artifact_id;
    TaskId task_id;
    PipelineId pipeline_id{0};
    std::uint64_t segment_id{0};
    WallTime committed_at;
    RecordingRange recording_range;
    ArtifactWallRange receive_wall_time_range;
    std::string format;
    std::uint64_t logical_bytes{0};
    std::uint64_t physical_bytes{0};
    DataImpact data_impact{DataImpact::None};
};

struct ArtifactFileInfo {
    ArtifactFileId file_id;
    std::string display_name;
    std::uint64_t size{0};
    std::string media_type;
    std::string etag;
};

struct ArtifactStreamCoverage {
    StreamId stream_id{0};
    std::uint64_t accepted{0};
    std::uint64_t written{0};
    std::uint64_t known_failed{0};
    std::uint64_t unknown{0};
    std::uint64_t first_sequence{0};
    std::uint64_t last_sequence{0};
};

struct ArtifactInfo {
    ArtifactSummary summary;
    std::vector<ArtifactFileInfo> files;
    // Accepted-set evidence; does not claim complete source/network coverage.
    std::vector<ArtifactStreamCoverage> coverage;
};

struct ArtifactQuery {
    std::optional<TaskId> task_id;
    std::optional<PipelineId> pipeline_id;
    std::optional<std::string> format;
    std::optional<RecordingRange> recording_range;
    std::optional<ArtifactWallRange> receive_wall_time_range;
    std::optional<ArtifactWallRange> committed_at;
    std::uint32_t limit{100};
    std::string cursor;
};

struct ArtifactPage {
    std::vector<ArtifactSummary> items;
    bool has_more{false};
    std::string next_cursor;
};

struct MutableBytes {
    std::byte* data{nullptr};
    std::size_t size{0};
};

class ArtifactReadHandle {
public:
    ArtifactReadHandle(const ArtifactReadHandle&) = delete;
    ArtifactReadHandle& operator=(const ArtifactReadHandle&) = delete;
    ArtifactReadHandle(ArtifactReadHandle&&) noexcept;
    ArtifactReadHandle& operator=(ArtifactReadHandle&&) noexcept;
    ~ArtifactReadHandle() noexcept;

    std::uint64_t size() const noexcept;
    // Blocking read execution only. Caller retains the output backing through
    // actual return. One in-flight call per handle; cancel is not completion.
    Result<std::size_t> read_at(std::uint64_t offset, MutableBytes output);
    void request_cancel() noexcept;

private:
    friend class artifact::ArtifactCatalog;
    explicit ArtifactReadHandle(std::shared_ptr<artifact::ReadBacking> backing) noexcept;
    std::shared_ptr<artifact::ReadBacking> backing_;
};

class ArtifactService {
public:
    static Result<std::unique_ptr<ArtifactService>> create(
        std::shared_ptr<artifact::ArtifactCatalog> catalog);
    Result<ArtifactPage> query_artifacts(const ArtifactQuery& query) const;
    Result<ArtifactInfo> get_artifact(ArtifactId artifact_id) const;
    // Filesystem work: execute on read capacity, never a control/event-loop lane.
    Result<ArtifactReadHandle> acquire_read(ArtifactId artifact_id, ArtifactFileId file_id);

private:
    explicit ArtifactService(std::shared_ptr<artifact::ArtifactCatalog> catalog) noexcept;
    std::shared_ptr<artifact::ArtifactCatalog> catalog_;
};

}  // namespace rodaq
