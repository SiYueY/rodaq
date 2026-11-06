#include "plugin/plugin_quarantine.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <thread>

namespace {
// Isolated executable: only the calling thread fails allocations. The worker
// enables the guard after Run has allocated its entry and started the thread.
thread_local const std::atomic<bool>* allocation_failure = nullptr;
std::atomic<std::size_t> denied_allocations{0};
}  // namespace

// Linker wrapping keeps the sanitizer's own new/delete interceptors intact.
extern "C" void* __real__Znwm(std::size_t size);
extern "C" void* __real__Znam(std::size_t size);

namespace {
void check_allocation() {
    if (allocation_failure != nullptr && allocation_failure->load(std::memory_order_acquire)) {
        denied_allocations.fetch_add(1);
        throw std::bad_alloc();
    }
}
}  // namespace

extern "C" void* __wrap__Znwm(std::size_t size) {
    check_allocation();
    return __real__Znwm(size);
}
extern "C" void* __wrap__Znam(std::size_t size) {
    check_allocation();
    return __real__Znam(size);
}

int main() {
    using namespace rodaq;
    using namespace std::chrono_literals;
    auto& manager = plugin::PluginQuarantine::process();
    std::atomic<bool> fail{false};
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto joined_call = std::make_shared<std::atomic<bool>>(false);
    allocation_failure = &fail;
    fail.store(true);
    bool injection_works = false;
    try {
        void* probe = ::operator new(1);
        ::operator delete(probe);
    } catch (const std::bad_alloc&) {
        injection_works = true;
    }
    fail.store(false);
    denied_allocations.store(0);
    auto outcome = manager.run(
        [&fail, release] {
            fail.store(true, std::memory_order_release);
            while (!release->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return rodaq::Result<void>();
        },
        std::chrono::steady_clock::now() + 100ms, "allocation-free timeout", joined_call);
    const bool timeout_guard_enabled = fail.load(std::memory_order_acquire);
    allocation_failure = nullptr;
    fail.store(false);
    const bool timed_out =
        outcome.ok() && outcome.value().status == plugin::SupervisedCallStatus::TimedOut;

    auto done = std::make_shared<std::atomic<bool>>(false);
    auto joined_worker = std::make_shared<std::atomic<bool>>(false);
    std::string reason(128, 'r');
    std::thread worker([release, done] {
        while (!release->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        done->store(true, std::memory_order_release);
    });
    allocation_failure = &fail;
    fail.store(true);
    const bool retained =
        manager.retain_worker(std::move(worker), release, done, std::move(reason), joined_worker);
    fail.store(false);
    allocation_failure = nullptr;
    release->store(true, std::memory_order_release);
    // Failed retention preserves the caller's thread; a moved-from thread is queryable.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    if (worker.joinable()) {
        worker.join();
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((!joined_call->load() || !joined_worker->load()) &&
           std::chrono::steady_clock::now() < deadline) {
        manager.reap_completed();
        std::this_thread::yield();
    }
    return injection_works && timeout_guard_enabled && timed_out && retained &&
                   joined_call->load() && joined_worker->load() && denied_allocations.load() == 0
               ? 0
               : 1;
}
