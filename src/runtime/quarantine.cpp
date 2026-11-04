#include "runtime/quarantine.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace rodaq::runtime {

namespace {

constexpr std::size_t kMaxRetainedCalls = 64;
constexpr std::size_t kMaxActivationRoots = 32;

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
    std::string reason;
};

struct QuarantineManager::ActivationSlot {
    bool reserved = false;
    std::shared_ptr<void> root;
    std::string reason;
};

QuarantineManager::QuarantineManager() noexcept : activations_(kMaxActivationRoots) {}

QuarantineManager& QuarantineManager::Process() noexcept {
    /*
     * Quarantined Plugin code may execute until process teardown. This owner is
     * intentionally process-lifetime; its fixed capacities bound retained
     * workers and activation roots.
     */
    static QuarantineManager* manager = new QuarantineManager();
#if defined(RODAQ_QUARANTINE_HAS_LSAN_IGNORE)
    __lsan_ignore_object(manager);
#endif
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
    std::string reason) {
    if (!operation) {
        return nonstd::make_unexpected(
            quarantine_error(ErrorCode::InvalidArgument, "Supervised Plugin call is empty"));
    }

    ReapCompleted();
    auto completion = std::make_shared<Completion>();
    std::unique_ptr<CallEntry> entry;
    try {
        entry = CreateCallEntry(completion, std::move(reason));
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
        calls_.push_back(std::move(entry));
    }
    CallEntry* entry_pointer = calls_.back().get();

    try {
        entry_pointer->worker =
            std::thread([completion, operation = std::move(operation)]() mutable {
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
                {
                    std::lock_guard<std::mutex> lock(completion->mutex);
                    completion->done = true;
                }
                completion->condition.notify_all();
            });
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        calls_.erase(std::find_if(
            calls_.begin(), calls_.end(),
            [entry_pointer](const std::unique_ptr<CallEntry>& candidate) {
                return candidate.get() == entry_pointer;
            }));
        --active_calls_;
        return nonstd::make_unexpected(quarantine_error(
            ErrorCode::Unavailable,
            std::string("Failed to start supervised Plugin call: ") + error.what()));
    }

    std::unique_ptr<CallEntry> local_entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = std::find_if(
            calls_.begin(), calls_.end(),
            [entry_pointer](const std::unique_ptr<CallEntry>& candidate) {
                return candidate.get() == entry_pointer;
            });
        if (found != calls_.end()) {
            local_entry = std::move(*found);
            calls_.erase(found);
        }
    }
    if (local_entry == nullptr) {
        return nonstd::make_unexpected(quarantine_error(
            ErrorCode::InternalError, "Supervised Plugin call ownership was lost"));
    }

    SupervisedCallOutcome outcome = WaitForCall(local_entry, deadline);
    if (outcome.status == SupervisedCallStatus::TimedOut) {
        std::lock_guard<std::mutex> lock(mutex_);
        calls_.push_back(std::move(local_entry));
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
    std::uint64_t activation_id, std::shared_ptr<void> root, std::string reason) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activation_id || activation_id > activations_.size() || root == nullptr) {
        return false;
    }
    ActivationSlot& slot = activations_[static_cast<std::size_t>(activation_id - 1U)];
    if (!slot.reserved) {
        return false;
    }
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
    std::lock_guard<std::mutex> manager_lock(mutex_);
    for (auto iterator = calls_.begin(); iterator != calls_.end();) {
        const bool complete = [&iterator] {
            std::lock_guard<std::mutex> completion_lock((*iterator)->completion->mutex);
            return (*iterator)->completion->done;
        }();
        if (!complete) {
            ++iterator;
            continue;
        }
        if ((*iterator)->worker.joinable()) {
            (*iterator)->worker.join();
        }
        iterator = calls_.erase(iterator);
        --active_calls_;
    }
}

std::size_t QuarantineManager::RetainedCallCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_.size();
}

std::size_t QuarantineManager::RetainedActivationCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(
        activations_.begin(), activations_.end(),
        [](const ActivationSlot& slot) { return slot.reserved || slot.root != nullptr; }));
}

}  // namespace rodaq::runtime
