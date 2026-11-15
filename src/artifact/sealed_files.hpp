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
    std::string digest;
};

// Single Commit-lane owner, retained through actual filesystem calls. Opens and
// pins the exact sealed regular-file set and computes hashes of actual bytes.
// No method calls Storage or establishes Ledger/Catalog committed authority.
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
    // Core supplies already validated/domain-encoded Manifest bytes. First
    // attempt only: failure/uncertainty requires Commit recovery, not overwrite.
    Result<void> install_manifest(std::string_view encoded_manifest);
    Result<void> sync_tree();
    // Rechecks pinned identities, exact flat entries and Volume identity.
    Result<void> verify() const;
    bool durable_tree() const noexcept;

private:
    struct State;
    explicit SealedFiles(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::artifact
