#ifndef RODAQ_PLUGIN_QUARANTINE_HPP_
#define RODAQ_PLUGIN_QUARANTINE_HPP_

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

namespace rodaq::plugin {

enum class SupervisedCallStatus {
    Completed,
    Failed,
    TimedOut,
};

struct SupervisedCallOutcome {
    SupervisedCallStatus status = SupervisedCallStatus::Failed;
    Status error;
};

class PluginQuarantine final {
public:
    static PluginQuarantine& process() noexcept;

    PluginQuarantine(const PluginQuarantine&) = delete;
    PluginQuarantine& operator=(const PluginQuarantine&) = delete;
    PluginQuarantine(PluginQuarantine&&) = delete;
    PluginQuarantine& operator=(PluginQuarantine&&) = delete;

    Result<SupervisedCallOutcome> run(
        std::function<Result<void>()> operation, std::chrono::steady_clock::time_point deadline,
        std::string reason, std::shared_ptr<std::atomic<bool>> joined = {});
    bool retain_worker(
        std::thread&& worker, std::shared_ptr<void> state, std::shared_ptr<std::atomic<bool>> done,
        std::string reason, std::shared_ptr<std::atomic<bool>> joined = {}) noexcept;

    Result<std::uint64_t> reserve_activation(std::string reason);
    bool retain_activation(
        std::uint64_t activation_id, std::shared_ptr<void> root, std::string reason,
        std::function<bool()> cleanup = {}) noexcept;
    void release_activation(std::uint64_t activation_id) noexcept;
    void reap_completed() noexcept;

    std::size_t retained_call_count() const noexcept;
    std::size_t retained_worker_count() const noexcept;
    std::size_t retained_activation_count() const noexcept;

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

    PluginQuarantine() noexcept;
    ~PluginQuarantine() = delete;

    std::unique_ptr<CallEntry> create_call_entry(
        std::shared_ptr<Completion> completion, std::string reason);
    SupervisedCallOutcome wait_for_call(
        std::unique_ptr<CallEntry>& entry, std::chrono::steady_clock::time_point deadline);

    void reap_activations() noexcept;

    mutable std::mutex mutex_;
    std::size_t active_calls_ = 0;
    std::size_t reaping_calls_ = 0;
    std::size_t reaping_workers_ = 0;
    std::array<std::unique_ptr<CallEntry>, kMaxRetainedCalls> calls_;
    std::array<std::optional<WorkerEntry>, kMaxRetainedCalls> retired_workers_{};
    std::array<ActivationSlot, 32> activations_{};
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_QUARANTINE_HPP_
