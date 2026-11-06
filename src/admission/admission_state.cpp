#include "admission/admission_state.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace rodaq {
namespace {

std::size_t stream_index(StreamHandle stream, std::size_t capacity) {
    const std::uint64_t encoded = stream.value() & 0xffffffffULL;
    if (encoded == 0 || encoded > capacity) {
        throw std::invalid_argument("Admission stream handle is outside the activation capacity");
    }
    return static_cast<std::size_t>(encoded - 1U);
}

}  // namespace

AdmissionState::AdmissionState(const DAQConfig& config)
: sequences_([&] {
      auto result = AcceptanceSequenceAllocator::create(config.resources.max_streams);
      if (!result.ok()) {
          throw std::runtime_error(std::string(result.status().message()));
      }
      return std::move(result.value());
  }()),
  stream_mutexes_(config.resources.max_streams) {}

void AdmissionState::register_stream(StreamHandle stream) {
    auto& mutex = stream_mutexes_[stream_index(stream, stream_mutexes_.size())];
    if (mutex != nullptr) {
        throw std::invalid_argument("Admission stream was already registered");
    }
    mutex = std::make_unique<std::mutex>();
}

bool AdmissionState::is_stream_registered(StreamHandle stream) const noexcept {
    try {
        const std::size_t index = stream_index(stream, stream_mutexes_.size());
        return stream_mutexes_[index] != nullptr;
    } catch (...) {
        return false;
    }
}

std::mutex& AdmissionState::stream_mutex(StreamHandle stream) {
    auto& mutex = stream_mutexes_[stream_index(stream, stream_mutexes_.size())];
    if (mutex == nullptr) {
        throw std::invalid_argument("Admission stream is not registered");
    }
    return *mutex;
}

bool AdmissionState::begin_submission() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ++active_submission_calls_;
    return true;
}

void AdmissionState::end_submission() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --active_submission_calls_;
    }
    condition_.notify_all();
}

bool AdmissionState::wait_for_drain(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_until(lock, deadline, [this] {
        return active_submission_calls_ == 0 && admission_inflight_ == 0;
    });
}

bool AdmissionState::wait_for_submissions(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_until(lock, deadline, [this] { return active_submission_calls_ == 0; });
}

void AdmissionState::open_gate() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gate_open_ = true;
    }
    condition_.notify_all();
}

void AdmissionState::close_gate() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gate_open_ = false;
    }
    condition_.notify_all();
}

bool AdmissionState::gate_open() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return gate_open_;
}

bool AdmissionState::can_allocate(StreamHandle stream) noexcept {
    return sequences_.can_allocate(stream);
}

Result<std::uint64_t> AdmissionState::allocate(StreamHandle stream) {
    return sequences_.allocate(stream);
}

void AdmissionState::notify_eligibility_changed() noexcept {
    eligibility_generation_.fetch_add(1, std::memory_order_acq_rel);
    condition_.notify_all();
}

void AdmissionState::record_inflight_acquired() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ++admission_inflight_;
}

void AdmissionState::record_inflight_released() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --admission_inflight_;
    }
    condition_.notify_all();
}

}  // namespace rodaq
