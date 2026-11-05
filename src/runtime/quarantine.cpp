#include "runtime/quarantine.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace rodaq::runtime {

namespace {

constexpr std::size_t kMaxRetainedCalls = 64;

Error quarantine_error(ErrorCode code, std::string message) {
    return Error(code, "runtime_quarantine", 0, std::move(message));
}

#if defined(__SANITIZE_ADDRESS__)
extern "C" void __lsan_ignore_object(const void*);
#define RODAQ_QUARANTINE_HAS_LSAN_IGNORE 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
extern "C" void __lsan_ignore_object(const void*);
#define RODAQ_QUARANTINE_HAS_LSAN_IGNORE 1
#endif
#endif

}  // namespace

struct QuarantineManager::Completion {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    bool threw = false;
    std::optional<Error> error;
};

struct QuarantineManager::CallEntry {
    std::shared_ptr<Completion> completion;
    std::thread worker;
    std::shared_ptr<std::atomic<bool>> joined;
    std::string reason;
};

struct QuarantineManager::WorkerEntry {
    std::thread worker;
    std::shared_ptr<void> state;
    std::shared_ptr<std::atomic<bool>> done;
    std::shared_ptr<std::atomic<bool>> joined;
    std::string reason;
};

QuarantineManager::QuarantineManager() noexcept = default;

QuarantineManager& QuarantineManager::Process() noexcept {
    /*
     * Quarantined Plugin code may execute until process teardown. This owner is
     * intentionally process-lifetime; its fixed capacities bound retained
     * workers and activation roots.
     */
    static QuarantineManager* manager = [] {
        auto* value = new (std::nothrow) QuarantineManager();
        if (value == nullptr) {
            std::abort();
        }
        return value;
    }();
#if defined(RODAQ_QUARANTINE_HAS_LSAN_IGNORE)
    __lsan_ignore_object(manager);
#endif
    // A single bounded maintenance worker joins eventually completed calls even
    // when no subsequent Runtime API call arrives. Its own lifetime is joined.
    struct Reaper final {
        explicit Reaper(QuarantineManager& owner)
        : worker([this, &owner] {
              std::unique_lock<std::mutex> lock(mutex);
              while (!closed) {
                  condition.wait_for(
                      lock, std::chrono::milliseconds(10), [this] { return closed; });
                  lock.unlock();
                  owner.ReapCompleted();
                  owner.ReapActivations();
                  lock.lock();
              }
              lock.unlock();
              // Process teardown still joins operations that finish during the
              // bounded final grace period. Truly blocked Plugin code remains
              // retained; completion never means an implicit detach.
              const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
              do {
                  owner.ReapCompleted();
                  owner.ReapActivations();
                  if (owner.RetainedCallCount() == 0 && owner.RetainedWorkerCount() == 0) {
                      owner.ReapCompleted();
                      break;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
              } while (std::chrono::steady_clock::now() < deadline);
          }) {}
        ~Reaper() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                closed = true;
                condition.notify_all();
            }
            worker.join();
        }
        std::mutex mutex;
        std::condition_variable condition;
        bool closed = false;
        std::thread worker;
    };
    static Reaper reaper(*manager);
    return *manager;
}

std::unique_ptr<QuarantineManager::CallEntry> QuarantineManager::CreateCallEntry(
    std::shared_ptr<Completion> completion, std::string reason) {
    try {
        auto entry = std::make_unique<CallEntry>();
        entry->completion = std::move(completion);
        entry->reason = std::move(reason);
        return entry;
    } catch (const std::bad_alloc&) {
        throw std::runtime_error("Memory exhausted while creating a supervised Plugin call");
    }
}

Result<SupervisedCallOutcome> QuarantineManager::Run(
    std::function<Result<void>()> operation, std::chrono::steady_clock::time_point deadline,
    std::string reason, std::shared_ptr<std::atomic<bool>> joined) {
    if (!operation) {
        return nonstd::make_unexpected(
            quarantine_error(ErrorCode::InvalidArgument, "Supervised Plugin call is empty"));
    }

    ReapCompleted();
    std::shared_ptr<Completion> completion;
    std::unique_ptr<CallEntry> entry;
    try {
        completion = std::make_shared<Completion>();
        entry = CreateCallEntry(completion, std::move(reason));
        entry->joined = std::move(joined);
    } catch (const std::exception& error) {
        return nonstd::make_unexpected(
            quarantine_error(ErrorCode::ResourceExhausted, error.what()));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_calls_ >= kMaxRetainedCalls) {
            return nonstd::make_unexpected(quarantine_error(
                ErrorCode::ResourceExhausted,
                "Supervised Plugin call quarantine capacity is exhausted"));
        }
        ++active_calls_;
    }

    try {
        entry->worker = std::thread([completion, operation = std::move(operation)]() mutable {
            try {
                Result<void> result = operation();
                std::lock_guard<std::mutex> lock(completion->mutex);
                if (!result.has_value()) {
                    completion->error.emplace(std::move(result.error()));
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(completion->mutex);
                completion->threw = true;
            }
            // Captured leases/destructors may execute Plugin code too. Dispose
            // them before publishing completion, so a reaper never joins a
            // thread blocked in an ostensibly "completed" capture destructor.
            operation = {};
            {
                std::lock_guard<std::mutex> lock(completion->mutex);
                completion->done = true;
            }
            completion->condition.notify_all();
        });
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        --active_calls_;
        return nonstd::make_unexpected(quarantine_error(
            ErrorCode::Unavailable,
            std::string("Failed to start supervised Plugin call: ") + error.what()));
    }

    SupervisedCallOutcome outcome = WaitForCall(entry, deadline);
    if (outcome.status == SupervisedCallStatus::TimedOut) {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            calls_.push_back(std::move(entry));
        } catch (const std::bad_alloc&) {
            --active_calls_;
            std::abort();
        }
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        --active_calls_;
    }
    return outcome;
}

SupervisedCallOutcome QuarantineManager::WaitForCall(
    std::unique_ptr<CallEntry>& entry, std::chrono::steady_clock::time_point deadline) {
    {
        std::unique_lock<std::mutex> lock(entry->completion->mutex);
        if (!entry->completion->condition.wait_until(
                lock, deadline, [&entry] { return entry->completion->done; })) {
            return SupervisedCallOutcome{SupervisedCallStatus::TimedOut, std::nullopt};
        }
    }
    if (entry->worker.joinable()) {
        entry->worker.join();
    }
    if (entry->joined != nullptr) {
        entry->joined->store(true, std::memory_order_release);
    }

    std::lock_guard<std::mutex> lock(entry->completion->mutex);
    if (entry->completion->threw) {
        return SupervisedCallOutcome{
            SupervisedCallStatus::Failed,
            quarantine_error(ErrorCode::InternalError, "Supervised Plugin call threw")};
    }
    if (entry->completion->error.has_value()) {
        return SupervisedCallOutcome{
            SupervisedCallStatus::Failed, std::move(entry->completion->error.value())};
    }
    return SupervisedCallOutcome{SupervisedCallStatus::Completed, std::nullopt};
}

bool QuarantineManager::RetainWorker(
    std::thread&& worker, std::shared_ptr<void> state, std::shared_ptr<std::atomic<bool>> done,
    std::string reason, std::shared_ptr<std::atomic<bool>> joined) noexcept {
    if (!worker.joinable() || state == nullptr || done == nullptr) {
        return false;
    }
    ReapCompleted();
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_calls_ >= kMaxRetainedCalls) {
            return false;
        }
        retired_workers_.push_back(WorkerEntry{
            std::move(worker), std::move(state), std::move(done), std::move(joined),
            std::move(reason)});
        ++active_calls_;
        return true;
    } catch (...) {
        return false;
    }
}

Result<std::uint64_t> QuarantineManager::ReserveActivation(std::string reason) {
    ReapCompleted();
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t index = 0; index < activations_.size(); ++index) {
        ActivationSlot& slot = activations_[index];
        if (!slot.reserved && slot.root == nullptr) {
            slot.reserved = true;
            slot.reason = std::move(reason);
            return static_cast<std::uint64_t>(index + 1U);
        }
    }
    return nonstd::make_unexpected(quarantine_error(
        ErrorCode::ResourceExhausted, "Quarantined Activation capacity is exhausted"));
}

bool QuarantineManager::RetainActivation(
    std::uint64_t activation_id, std::shared_ptr<void> root, std::string reason,
    std::function<bool()> cleanup) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activation_id || activation_id > activations_.size() || root == nullptr) {
        return false;
    }
    ActivationSlot& slot = activations_[static_cast<std::size_t>(activation_id - 1U)];
    if (!slot.reserved) {
        return false;
    }
    slot.cleanup = std::move(cleanup);
    slot.root = std::move(root);
    slot.reason = std::move(reason);
    return true;
}

void QuarantineManager::ReleaseActivation(std::uint64_t activation_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activation_id || activation_id > activations_.size()) {
        return;
    }
    ActivationSlot& slot = activations_[static_cast<std::size_t>(activation_id - 1U)];
    if (slot.root == nullptr) {
        slot = ActivationSlot{};
    }
}

void QuarantineManager::ReapCompleted() noexcept {
    // Claim entries under the manager mutex. Joining and releasing captures may
    // reenter the manager, so neither may happen while that mutex is held.
    std::array<std::unique_ptr<CallEntry>, kMaxRetainedCalls> calls;
    std::array<std::optional<WorkerEntry>, kMaxRetainedCalls> workers;
    std::size_t call_count = 0;
    std::size_t worker_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = calls_.begin(); it != calls_.end();) {
            bool done = false;
            {
                std::lock_guard<std::mutex> completion_lock((*it)->completion->mutex);
                done = (*it)->completion->done;
            }
            if (done && (*it)->worker.get_id() != std::this_thread::get_id()) {
                calls[call_count++] = std::move(*it);
                it = calls_.erase(it);
                ++reaping_calls_;
            } else {
                ++it;
            }
        }
        for (auto it = retired_workers_.begin(); it != retired_workers_.end();) {
            if (it->done->load(std::memory_order_acquire) &&
                it->worker.get_id() != std::this_thread::get_id()) {
                workers[worker_count++].emplace(std::move(*it));
                it = retired_workers_.erase(it);
                ++reaping_workers_;
            } else {
                ++it;
            }
        }
    }
    for (std::size_t index = 0; index < call_count; ++index) {
        calls[index]->worker.join();
        if (calls[index]->joined != nullptr) {
            calls[index]->joined->store(true, std::memory_order_release);
        }
        calls[index].reset();
        std::lock_guard<std::mutex> lock(mutex_);
        --reaping_calls_;
        --active_calls_;
    }
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers[index]->worker.join();
        if (workers[index]->joined != nullptr) {
            workers[index]->joined->store(true, std::memory_order_release);
        }
        workers[index].reset();
        std::lock_guard<std::mutex> lock(mutex_);
        --reaping_workers_;
        --active_calls_;
    }
}

void QuarantineManager::ReapActivations() noexcept {
    for (std::size_t index = 0; index < activations_.size(); ++index) {
        std::shared_ptr<void> root;
        std::function<bool()> cleanup;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ActivationSlot& slot = activations_[index];
            if (slot.root == nullptr || !slot.cleanup || slot.reaping) {
                continue;
            }
            try {
                cleanup = slot.cleanup;
            } catch (...) {
                continue;
            }
            slot.reaping = true;
            root = slot.root;
        }
        bool complete = false;
        try {
            complete = cleanup();
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ActivationSlot& slot = activations_[index];
            if (complete) {
                // root keeps destruction outside the manager mutex.
                slot = ActivationSlot{};
            } else {
                slot.reaping = false;
            }
        }
    }
}

std::size_t QuarantineManager::RetainedCallCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_.size() + reaping_calls_;
}

std::size_t QuarantineManager::RetainedWorkerCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return retired_workers_.size() + reaping_workers_;
}

std::size_t QuarantineManager::RetainedActivationCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(
        activations_.begin(), activations_.end(),
        [](const ActivationSlot& slot) { return slot.reserved || slot.root != nullptr; }));
}

}  // namespace rodaq::runtime
