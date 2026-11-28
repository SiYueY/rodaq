#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "../persistence/atomic_file.hpp"

namespace rodaq::artifact {

struct VolumeId {
    std::array<std::uint8_t, 16> value{};
    friend bool operator==(VolumeId left, VolumeId right) noexcept {
        return left.value == right.value;
    }
};

Result<VolumeId> parse_volume_id(std::string_view text);
std::string to_string(VolumeId id);

enum class VolumeOpenMode { Initialize, Existing };

struct VolumeRuntimeIdentity {
    std::uint64_t device{0};
    std::uint64_t inode{0};
    std::uint64_t mount_id{0};
    std::string filesystem_type;
    std::string mount_point;
    std::string mount_options;
    std::string super_options;
};

// One Core owner of a real output root. Mount/marker failures latch protection
// until explicit deployment re-admission. All filesystem methods run outside
// control/Admission/structure locks.
class ManagedVolume {
public:
    static Result<std::shared_ptr<ManagedVolume>> open(
        const std::filesystem::path& root, VolumeOpenMode mode, VolumeId expected_identity = {},
        persistence::FileFaultHook faults = {});
    ManagedVolume(const ManagedVolume&) = delete;
    ManagedVolume& operator=(const ManagedVolume&) = delete;
    ~ManagedVolume() noexcept;

    VolumeId identity() const noexcept;
    const VolumeRuntimeIdentity& runtime_identity() const noexcept;
    const std::filesystem::path& root() const noexcept;
    const std::filesystem::path& artifacts_directory() const noexcept;
    StatusCode protection() const noexcept;
    Result<void> verify() const;
    Result<std::uint64_t> available_bytes() const;
    struct SpaceStats {
        std::uint64_t total_bytes{0};
        std::uint64_t available_bytes{0};
    };
    Result<SpaceStats> space_stats() const;
    // Generates a random non-zero 64-bit segment_id unused in .rodaq/artifacts/.
    Result<std::uint64_t> allocate_segment_id() const;
    // Lazy Writer work only. No-replace staging directory creation (<id>.tmp).
    Result<std::filesystem::path> create_staging(std::uint64_t segment_id);

private:
    struct State;
    explicit ManagedVolume(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::artifact
