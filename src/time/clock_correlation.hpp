#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>

#include "clock.hpp"
#include "playback_anchor_store.hpp"

namespace rodaq::time {

struct ClockPolicy {
    std::uint64_t allowed_error_ns{1000000};
    std::uint32_t epoch_reservation_count{256};
};

// The DAQ composition schedules bounded periodic observations. This owner has
// no implicit threads; snapshots never wait for filesystem or observer work.
class ClockCorrelation {
public:
    static Result<std::unique_ptr<ClockCorrelation>> create(
        const std::filesystem::path& directory, const BootIdentity& boot,
        const ClockSample& initial_sample, BootArtifactEvidence artifacts, ClockPolicy policy = {},
        persistence::FileFaultHook faults = {});

    ClockCorrelation(const ClockCorrelation&) = delete;
    ClockCorrelation& operator=(const ClockCorrelation&) = delete;

    PlaybackAnchor anchor() const noexcept;
    Result<ClockSnapshot> snapshot() const;
    Result<void> observe(const ClockSample& sample);

private:
    ClockCorrelation(
        std::unique_ptr<PlaybackAnchorStore> store, const ClockSample& initial,
        ClockPolicy policy) noexcept;
    void protect(StatusCode failure);

    std::unique_ptr<PlaybackAnchorStore> store_;
    const ClockPolicy policy_;
    EpochRange range_;
    std::uint64_t next_epoch_;
    // Observation execution is serial; this lock is never used by readers.
    std::mutex observation_;
    mutable std::mutex snapshot_mutex_;
    ClockSnapshot snapshot_;
    StatusCode failure_{StatusCode::Ok};
};

}  // namespace rodaq::time
