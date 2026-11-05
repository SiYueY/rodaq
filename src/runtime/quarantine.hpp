#ifndef RODAQ_RUNTIME_QUARANTINE_HPP_
#define RODAQ_RUNTIME_QUARANTINE_HPP_

#include <rodaq/result.hpp>
#include <chrono>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace rodaq::runtime {

enum class SupervisedCallStatus {
    Completed,
    Failed,
    TimedOut,
};

struct SupervisedCallOutcome {
    SupervisedCallStatus status = SupervisedCallStatus::Failed;
    std::optional<Error> error;
};

class QuarantineManager final {
public:
    static QuarantineManager& Process() noexcept;

    QuarantineManager(const QuarantineManager&) = delete;
    QuarantineManager& operator=(const QuarantineManager&) = delete;
    QuarantineManager(QuarantineManager&&) = delete;
    QuarantineManager& operator=(QuarantineManager&&) = delete;

    Result<SupervisedCallOutcome> Run(
        std::function<Result<void>()> operation, std::chrono::steady_clock::time_point deadline,
        std::string reason, std::shared_ptr<std::atomic<bool>> joined = {});
    bool RetainWorker(
        std::thread&& worker, std::shared_ptr<void> state, std::shared_ptr<std::atomic<bool>> done,
        std::string reason, std::shared_ptr<std::atomic<bool>> joined = {}) noexcept;

    Result<std::uint64_t> ReserveActivation(std::string reason);
    bool RetainActivation(
        std::uint64_t activation_id, std::shared_ptr<void> root, std::string reason,
        std::function<bool()> cleanup = {}) noexcept;
    void ReleaseActivation(std::uint64_t activation_id) noexcept;
    void ReapCompleted() noexcept;

    std::size_t RetainedCallCount() const noexcept;
    std::size_t RetainedWorkerCount() const noexcept;
    std::size_t RetainedActivationCount() const noexcept;

private:
    struct Completion;
    struct CallEntry;
    struct WorkerEntry {
        std::thread worker;
        std::shared_ptr<void> state;
        std::shared_ptr<std::atomic<bool>> done;
        std::shared_ptr<std::atomic<bool>> joined;
        std::string reason;
    };
    static constexpr std::size_t kMaxRetainedCalls = 64;
    struct ActivationSlot {
        bool reserved = false;
        std::shared_ptr<void> root;
        std::string reason;
        std::function<bool()> cleanup;
        bool reaping = false;
    };

    QuarantineManager() noexcept;
    ~QuarantineManager() = delete;

    std::unique_ptr<CallEntry> CreateCallEntry(
        std::shared_ptr<Completion> completion, std::string reason);
    SupervisedCallOutcome WaitForCall(
        std::unique_ptr<CallEntry>& entry, std::chrono::steady_clock::time_point deadline);

    void ReapActivations() noexcept;

    mutable std::mutex mutex_;
    std::size_t active_calls_ = 0;
    std::size_t reaping_calls_ = 0;
    std::size_t reaping_workers_ = 0;
    std::array<std::unique_ptr<CallEntry>, kMaxRetainedCalls> calls_;
    std::array<std::optional<WorkerEntry>, kMaxRetainedCalls> retired_workers_{};
    std::array<ActivationSlot, 32> activations_{};
};

}  // namespace rodaq::runtime

#endif  // RODAQ_RUNTIME_QUARANTINE_HPP_
