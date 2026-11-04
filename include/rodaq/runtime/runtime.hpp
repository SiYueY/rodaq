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

    Result<void> Configure();
    Result<void> Start();
    Result<void> Stop();

    RuntimeState State() const noexcept;
    Result<rodaq_runtime_lifecycle_snapshot> LifecycleSnapshot() const;
    Result<Error> PrimaryFailure() const;
    Result<std::vector<Error>> SecondaryFailures() const;
    std::uint64_t SecondaryFailureOverflowCount() const noexcept;
    Result<RuntimeStatsSnapshot> Stats() const;
    Result<PayloadPoolStats> PayloadStats() const;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq::runtime

#endif  // RODAQ_RUNTIME_RUNTIME_HPP_
