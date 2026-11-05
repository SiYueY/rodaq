#ifndef RODAQ_RUNTIME_FAILURE_SIGNAL_HPP_
#define RODAQ_RUNTIME_FAILURE_SIGNAL_HPP_

#include <condition_variable>
#include <mutex>

namespace rodaq::runtime {

// Activation callbacks retain only this mailbox, never a Runtime pointer.
// One bit coalesces any number of failures; it owns no activation resources.
struct FailureSignal final {
    void notify() noexcept;
    void close() noexcept;

    std::mutex mutex;
    std::condition_variable condition;
    bool requested = false;
    bool closed = false;
};

}  // namespace rodaq::runtime

#endif  // RODAQ_RUNTIME_FAILURE_SIGNAL_HPP_
