#ifndef RODAQ_DAQ_DAQ_IMPL_HPP_
#define RODAQ_DAQ_DAQ_IMPL_HPP_

#include "activation/activation_state.hpp"
#include "daq/failure_mailbox.hpp"
#include "plugin/plugin_quarantine.hpp"

#include <rodaq/daq.hpp>
#include <rodaq/plugin/abi/acquisition.h>
#include <rodaq/plugin/abi/storage.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rodaq {

Status daq_status(StatusCode status_code, std::string message);

enum class AcquisitionActivationState {
    Empty,
    Created,
    Bound,
    Configured,
    StartAttempted,
    Started,
    Quiesced,
    Stopped,
    Quarantined,
};

enum class StorageActivationState {
    Empty,
    Created,
    Bound,
    Configured,
    Opened,
    Closed,
    Quarantined,
};

struct BoundedCallOutcome {
    enum class Outcome {
        Completed,
        Failed,
        TimedOut,
    };

    Outcome status = Outcome::Failed;
    Status error;
};

template <typename Callable>
BoundedCallOutcome supervise_call(
    Callable callable, std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<std::atomic<bool>> joined = {}) noexcept {
    auto managed = plugin::PluginQuarantine::process().run(
        std::function<Result<void>()>(std::move(callable)), deadline, "DAQ Plugin control call",
        std::move(joined));
    if (!managed.ok()) {
        return BoundedCallOutcome{BoundedCallOutcome::Outcome::Failed, managed.status()};
    }
    switch (managed.value().status) {
        case plugin::SupervisedCallStatus::Completed:
            return BoundedCallOutcome{BoundedCallOutcome::Outcome::Completed, {}};
        case plugin::SupervisedCallStatus::Failed:
            return BoundedCallOutcome{
                BoundedCallOutcome::Outcome::Failed, std::move(managed.value().error)};
        case plugin::SupervisedCallStatus::TimedOut:
            return BoundedCallOutcome{BoundedCallOutcome::Outcome::TimedOut, {}};
    }
    return BoundedCallOutcome{
        BoundedCallOutcome::Outcome::Failed,
        daq_status(StatusCode::InternalError, "Unknown supervised call outcome")};
}

class DAQ::Impl final {
    // Ownership is logical: readers may acquire the mutex while Plugin I/O
    // runs, but another lifecycle operation must wait for this owner.
    class LifecycleOwner final {
    public:
        explicit LifecycleOwner(Impl& owner);
        ~LifecycleOwner();

    private:
        Impl& owner_;
        std::unique_lock<std::mutex> lock_;
    };

public:
    explicit Impl(DAQConfig config);
    ~Impl();

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
    template <typename Callable>
    auto outside_lifecycle(Callable callable) -> decltype(callable()) {
        struct Unlock final {
            std::mutex& mutex;
            explicit Unlock(std::mutex& value) : mutex(value) { mutex.unlock(); }
            ~Unlock() { mutex.lock(); }
        } unlock(lifecycle_mutex_);
        return callable();
    }

    struct PendingCall final {
        std::atomic<bool> done{false};
        std::shared_ptr<std::atomic<bool>> joined = std::make_shared<std::atomic<bool>>(false);
        Result<void> result;
        std::string site;
    };

    template <typename Callable>
    BoundedCallOutcome run_bounded_call(
        const char* site, Callable callable,
        std::chrono::steady_clock::time_point deadline) noexcept {
        if (pending_call_ != nullptr) {
            if (pending_call_->site != site ||
                !pending_call_->joined->load(std::memory_order_acquire)) {
                return {BoundedCallOutcome::Outcome::TimedOut, {}};
            }
            auto pending = std::move(pending_call_);
            if (!pending->result.ok()) {
                recovery_abandoned_ = true;
                return {BoundedCallOutcome::Outcome::Failed, pending->result.status()};
            }
            return {BoundedCallOutcome::Outcome::Completed, {}};
        }
        auto pending = std::make_shared<PendingCall>();
        pending->site = site;
        auto outcome = outside_lifecycle([&] {
            return supervise_call(
                [pending, callable = std::move(callable)]() mutable {
                    try {
                        pending->result = callable();
                    } catch (...) {
                        pending->result =
                            daq_status(StatusCode::InternalError, "Plugin control call threw");
                    }
                    pending->done.store(true, std::memory_order_release);
                    return pending->result;
                },
                deadline, pending->joined);
        });
        if (outcome.status == BoundedCallOutcome::Outcome::TimedOut) {
            pending_call_ = std::move(pending);
        } else if (outcome.status == BoundedCallOutcome::Outcome::Failed) {
            recovery_abandoned_ = true;
        }
        return outcome;
    }

    void failure_loop() noexcept;

    template <typename Callable>
    Result<void> supervise_start_call(Callable callable) {
        auto outcome = run_bounded_call(
            "activation-call", std::move(callable),
            std::chrono::steady_clock::now() + options_.stop_progress_timeout);
        if (outcome.status == BoundedCallOutcome::Outcome::TimedOut) {
            start_quarantine_required_ = true;
            return daq_status(StatusCode::Timeout, "Plugin activation call timed out");
        }
        if (outcome.status == BoundedCallOutcome::Outcome::Failed) {
            return std::move(outcome.error);
        }
        return {};
    }

    template <typename T, typename Callable>
    Result<T> supervise_start_value(Callable callable) {
        auto value = std::make_shared<std::optional<T>>();
        auto error = std::make_shared<Status>();
        auto outcome = run_bounded_call(
            "activation-value",
            [callable = std::move(callable), value, error]() mutable -> Result<void> {
                auto result = callable();
                if (result.ok()) {
                    value->emplace(std::move(result.value()));
                } else {
                    *error = std::move(result).status();
                }
                return {};
            },
            std::chrono::steady_clock::now() + options_.stop_progress_timeout);
        if (outcome.status == BoundedCallOutcome::Outcome::TimedOut) {
            start_quarantine_required_ = true;
            return daq_status(StatusCode::Timeout, "Plugin activation call timed out");
        }
        if (outcome.status == BoundedCallOutcome::Outcome::Failed) {
            return std::move(outcome.error);
        }
        if (!error->ok()) {
            return std::move(*error);
        }
        return std::move(value->value());
    }

    Result<void> start_activation();
    Result<void> stop_internal();
    void abort_start() noexcept;
    bool recover_activation();
    void transfer_quarantine();
    Result<void> quarantine_failure();
    void capture_activation_outcome() noexcept;
    void release_activation_slot() noexcept;

    DAQConfig options_;
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_condition_;
    bool lifecycle_owned_ = false;
    std::shared_ptr<FailureMailbox> failure_signal_ = std::make_shared<FailureMailbox>();
    std::thread failure_worker_;
    DAQState state_ = DAQState::Constructed;
    std::uint64_t activation_generation_ = 0;
    std::shared_ptr<ActivationState> active_;
    std::shared_ptr<ActivationState> quarantined_;
    std::optional<std::uint64_t> activation_slot_;
    std::vector<AcquisitionActivationState> acquisition_states_;
    StorageActivationState storage_state_ = StorageActivationState::Empty;
    bool storage_open_ = false;
    bool start_quarantine_required_ = false;
    bool storage_flushed_ = false;
    bool aborting_ = false;
    bool recovery_abandoned_ = false;
    bool recovering_ = false;
    std::shared_ptr<PendingCall> pending_call_;
    std::optional<DAQStats> last_stats_;
    std::optional<PayloadStats> last_payload_stats_;
    Status last_primary_failure_;
    std::vector<Status> last_secondary_failures_;
    std::uint64_t last_secondary_failure_overflow_count_ = 0;
    std::chrono::nanoseconds effective_storage_handoff_timeout_{};
    std::chrono::nanoseconds effective_storage_flush_timeout_{};
    std::chrono::nanoseconds effective_storage_close_timeout_{};
};

}  // namespace rodaq

#endif  // RODAQ_DAQ_DAQ_IMPL_HPP_
