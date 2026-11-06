#ifndef RODAQ_DAQ_HPP_
#define RODAQ_DAQ_HPP_

#include <rodaq/daq_config.hpp>
#include <rodaq/daq_stats.hpp>
#include <rodaq/export.hpp>
#include <rodaq/result.hpp>
#include <rodaq/status.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace rodaq {

enum class DAQState {
    Constructed,
    Configured,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

struct DAQLifecycleSnapshot {
    DAQState state = DAQState::Constructed;
    bool normal_acquisition_allowed = false;
    std::uint64_t generation = 0;
};

class RODAQ_EXPORT DAQ final {
public:
    explicit DAQ(DAQConfig config);
    DAQ(const DAQ&) = delete;
    DAQ& operator=(const DAQ&) = delete;
    DAQ(DAQ&&) = delete;
    DAQ& operator=(DAQ&&) = delete;
    ~DAQ();

    Result<void> configure();
    Result<void> start();
    Result<void> stop();

    DAQState state() const noexcept;
    Result<DAQLifecycleSnapshot> lifecycle_snapshot() const;
    Status primary_failure() const;
    Result<std::vector<Status>> secondary_failures() const;
    std::uint64_t secondary_failure_overflow_count() const noexcept;
    Result<DAQStats> stats() const;
    Result<PayloadStats> payload_stats() const;

private:
    class RODAQ_LOCAL Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq

#endif  // RODAQ_DAQ_HPP_
