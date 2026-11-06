#ifndef RODAQ_DAQ_FAILURE_MAILBOX_HPP_
#define RODAQ_DAQ_FAILURE_MAILBOX_HPP_

#include <condition_variable>
#include <mutex>

namespace rodaq {

// Activation callbacks retain only this mailbox, never a DAQ pointer.
// One bit coalesces any number of failures; it owns no activation resources.
struct FailureMailbox final {
    void notify() noexcept;
    void close() noexcept;

    std::mutex mutex;
    std::condition_variable condition;
    bool requested = false;
    bool closed = false;
};

}  // namespace rodaq

#endif  // RODAQ_DAQ_FAILURE_MAILBOX_HPP_
