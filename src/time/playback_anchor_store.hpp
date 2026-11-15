#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "clock.hpp"
#include "../persistence/atomic_file.hpp"

namespace rodaq::time {

enum class BootArtifactState { Absent, Present, Unknown };

struct BootArtifactEvidence {
    // Absent requires no current-boot recording facts, including staging and
    // recovery evidence, rather than merely an empty normal Artifact Catalog.
    BootArtifactState state{BootArtifactState::Unknown};
    // Present requires the common anchor of validated current-boot Artifacts.
    // Conflicting Artifact anchors must already fail the caller's reconciliation.
    PlaybackAnchor anchor;
};

struct EpochRange {
    std::uint64_t begin{0};
    std::uint64_t end_exclusive{0};
};

// One Core-owned time namespace. The composition prepares its durable directory.
// An exclusive file lock covers this owner's lifetime. No native threads are owned.
class PlaybackAnchorStore {
public:
    static Result<std::unique_ptr<PlaybackAnchorStore>> open(
        const std::filesystem::path& directory, const BootIdentity& boot,
        const ClockSample& initial_sample, BootArtifactEvidence artifacts,
        std::uint32_t epoch_reservation_count = 256, persistence::FileFaultHook faults = {});

    PlaybackAnchorStore(const PlaybackAnchorStore&) = delete;
    PlaybackAnchorStore& operator=(const PlaybackAnchorStore&) = delete;
    ~PlaybackAnchorStore() noexcept;

    PlaybackAnchor anchor() const noexcept;
    EpochRange initial_range() const noexcept;
    Result<EpochRange> reserve_epochs();

private:
    struct State;
    explicit PlaybackAnchorStore(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::time
