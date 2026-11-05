#ifndef RODAQ_RUNTIME_RUNTIME_HPP_
#define RODAQ_RUNTIME_RUNTIME_HPP_

// Public application-facing Runtime API Candidate.

#include <rodaq/plugin/abi/runtime.h>
#include <rodaq/result.hpp>
#include <rodaq/runtime/export.hpp>
#include <rodaq/runtime/logging.hpp>
#include <rodaq/runtime/payload_pool.hpp>
#include <rodaq/runtime/runtime_config.hpp>
#include <rodaq/runtime/runtime_stats.hpp>

#include <memory>
#include <vector>

namespace rodaq::runtime {

enum class RuntimeState {
    Constructed,
    Configured,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

class RODAQ_RUNTIME_EXPORT Runtime final {
public:
    explicit Runtime(RuntimeConfig config);
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
    ~Runtime();

    Result<void> configure();
    Result<void> start();
    Result<void> stop();

    RuntimeState state() const noexcept;
    Result<rodaq_runtime_lifecycle_snapshot> lifecycle_snapshot() const;
    Result<Error> primary_failure() const;
    Result<std::vector<Error>> secondary_failures() const;
    std::uint64_t secondary_failure_overflow_count() const noexcept;
    Result<RuntimeStatsSnapshot> stats() const;
    Result<PayloadPoolStats> payload_stats() const;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq::runtime

#endif  // RODAQ_RUNTIME_RUNTIME_HPP_
