#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/storage/types.hpp>

#include "managed_volume.hpp"

namespace rodaq::artifact {

struct FileEvidence {
    std::string relative_path;
    std::uint64_t physical_size{0};
};

class SegmentCommitter;

// Single Commit-lane owner, retained through actual filesystem calls. Validates
// the flat regular-file set and exact physical sizes without mandatory full-file
// SHA-256 readback, writes manifest.json, and performs same-parent atomic
// no-replace publication (<id>.tmp -> <id>).
class SealedFiles {
public:
    static Result<std::unique_ptr<SealedFiles>> open(
        std::shared_ptr<ManagedVolume> volume, std::uint64_t segment_id,
        const storage::SegmentSealResult& seal, const storage::PrepareConfig& limits,
        std::uint64_t manifest_byte_limit, persistence::FileFaultHook faults = {});
    SealedFiles(const SealedFiles&) = delete;
    SealedFiles& operator=(const SealedFiles&) = delete;
    ~SealedFiles() noexcept;

    const std::vector<FileEvidence>& evidence() const noexcept;
    std::uint64_t storage_bytes() const noexcept;
    std::uint64_t manifest_bytes() const noexcept;
    const std::filesystem::path& staging_directory() const noexcept;
    Result<void> install_manifest(std::string_view encoded_manifest);
    Result<void> sync_tree();
    Result<void> verify() const;
    bool durable_tree() const noexcept;
    std::uint64_t segment_id() const noexcept;
    VolumeId volume_id() const noexcept;

private:
    friend class SegmentCommitter;
    Result<void> publish_directory();
    struct State;
    explicit SealedFiles(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::artifact
