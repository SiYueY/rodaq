#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "../persistence/atomic_file.hpp"

namespace rodaq::artifact {

struct SegmentIdRange {
    std::uint64_t begin{0};
    std::uint64_t end_exclusive{0};
};

enum class SegmentIdOpenMode { NewDeployment, Existing };

// Deployment-wide Segment ID authority, independent of Task/Volume GC. The
// composition durably prepares this dedicated namespace. NewDeployment is
// permitted only with verified absence of all prior deployment recording facts.
class SegmentIdStore {
public:
    static Result<std::unique_ptr<SegmentIdStore>> open(
        const std::filesystem::path& directory, SegmentIdOpenMode mode,
        persistence::FileFaultHook faults = {});
    SegmentIdStore(const SegmentIdStore&) = delete;
    SegmentIdStore& operator=(const SegmentIdStore&) = delete;
    ~SegmentIdStore() noexcept;

    // Runs on persistence execution, never Admission. Only durable successful
    // ranges may be consumed. Restarts abandon the entire previous range tail.
    // Concurrent reservations serialize through actual publication; callers
    // retain their execution owners while waiting for another persistence call.
    Result<SegmentIdRange> reserve(std::uint32_t count);
    Result<void> reconcile();

private:
    struct State;
    explicit SegmentIdStore(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::artifact
