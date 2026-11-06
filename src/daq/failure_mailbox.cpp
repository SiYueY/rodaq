#include "daq/failure_mailbox.hpp"

namespace rodaq {

void FailureMailbox::notify() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (!closed) {
        requested = true;
        condition.notify_one();
    }
}

void FailureMailbox::close() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    closed = true;
    condition.notify_all();
}

}  // namespace rodaq
