#pragma once

#include <chrono>
#include <cstddef>
#include <memory>

#include <rodaq/result.hpp>

namespace rodaq {

class Work {
public:
    virtual ~Work() noexcept = default;
    virtual void run() noexcept = 0;
};

struct LaneSnapshot {
    std::size_t worker_count{0};
    std::size_t capacity{0};
    std::size_t queued{0};
    std::size_t running{0};
    bool accepting{false};
};

// Composition supplies separate lanes for domains that must not starve each
// other. Queue storage is fixed; each submitted frame owns all borrowed inputs.
// No detached threads. The owner must retain this lane after a shutdown timeout
// and retry shutdown after actual work ends, or let the process supervisor act.
// Destruction requires completed shutdown; it never waits on unknown work.
class ExecutionLane {
public:
    static Result<std::shared_ptr<ExecutionLane>> create(std::size_t workers, std::size_t capacity);
    ExecutionLane(const ExecutionLane&) = delete;
    ExecutionLane& operator=(const ExecutionLane&) = delete;
    ~ExecutionLane() noexcept;

    Result<void> submit(std::shared_ptr<Work> work);
    LaneSnapshot snapshot() const;
    Result<void> shutdown(std::chrono::steady_clock::time_point deadline);

private:
    struct State;
    explicit ExecutionLane(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq
