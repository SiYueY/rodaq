#include "runtime/failure_signal.hpp"

namespace rodaq::runtime {

void FailureSignal::notify() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (!closed) {
        requested = true;
        condition.notify_one();
    }
}

void FailureSignal::close() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    closed = true;
    condition.notify_all();
}

}  // namespace rodaq::runtime
