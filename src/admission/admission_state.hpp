#ifndef RODAQ_ADMISSION_ADMISSION_STATE_HPP_
#define RODAQ_ADMISSION_ADMISSION_STATE_HPP_

#include "admission/acceptance_sequence.hpp"
#include "canonical/handle.hpp"

#include <rodaq/daq_config.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace rodaq {

class AdmissionState final {
public:
    explicit AdmissionState(const DAQConfig& config);

    AdmissionState(const AdmissionState&) = delete;
    AdmissionState& operator=(const AdmissionState&) = delete;
    AdmissionState(AdmissionState&&) = delete;
    AdmissionState& operator=(AdmissionState&&) = delete;

    void register_stream(StreamHandle stream);
    bool is_stream_registered(StreamHandle stream) const noexcept;
    std::mutex& stream_mutex(StreamHandle stream);

    bool begin_submission() noexcept;
    void end_submission() noexcept;
    bool wait_for_drain(std::chrono::steady_clock::time_point deadline);
    bool wait_for_submissions(std::chrono::steady_clock::time_point deadline);

    void open_gate() noexcept;
    void close_gate() noexcept;
    bool gate_open() const noexcept;
    bool gate_open_locked() const noexcept { return gate_open_; }

    bool can_allocate(StreamHandle stream) noexcept;
    Result<std::uint64_t> allocate(StreamHandle stream);
    std::uint64_t eligibility_generation() const noexcept {
        return eligibility_generation_.load(std::memory_order_acquire);
    }
    void notify_eligibility_changed() noexcept;

    void record_inflight_acquired() noexcept;
    void record_inflight_released() noexcept;
    void record_inflight_acquired_locked() noexcept { ++admission_inflight_; }
    void record_inflight_released_locked() noexcept { --admission_inflight_; }

    std::mutex& mutex() noexcept { return mutex_; }
    void notify_all() noexcept { condition_.notify_all(); }

private:
    AcceptanceSequenceAllocator sequences_;
    std::vector<std::unique_ptr<std::mutex>> stream_mutexes_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool gate_open_ = false;
    std::atomic<std::uint64_t> eligibility_generation_{0};
    std::uint64_t active_submission_calls_ = 0;
    std::uint64_t admission_inflight_ = 0;
};

}  // namespace rodaq

#endif  // RODAQ_ADMISSION_ADMISSION_STATE_HPP_
