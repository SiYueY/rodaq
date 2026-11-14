#include <rodaq/plugin/instance.hpp>

#include "lifecycle.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <time.h>
#include <utility>

namespace rodaq::plugin {
namespace {

thread_local std::array<InstanceState*, 64> current_calls{};
thread_local std::size_t current_call_count = 0;
thread_local ManagerState* lifecycle_manager = nullptr;
thread_local InstanceState* lifecycle_instance = nullptr;

rodaq_status_code_t query_service(
    rodaq_service_registry_context_t* context, const rodaq_interface_query_t* query,
    rodaq_interface_reference_t* reference, rodaq_error_t* error) noexcept {
    if (reference != nullptr) {
        *reference = {};
    }
    abi::write_error(error, {});
    if (context == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const auto& access = *reinterpret_cast<InstanceServices*>(context);
    return registry_query(*access.registry, query, reference, error);
}

rodaq_status_code_t enter_service(
    rodaq_service_registry_context_t* context, rodaq_error_t* error) noexcept {
    abi::write_error(error, {});
    if (context == nullptr || !abi::valid_error_input(error)) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    auto& instance = *reinterpret_cast<InstanceServices*>(context)->instance;
    try {
        std::lock_guard<std::mutex> lock(instance.mutex);
        if (instance.phase != PluginInstancePhase::Open &&
            instance.phase != PluginInstancePhase::Initializing) {
            return RODAQ_STATUS_INVALID_STATE;
        }
        const auto thread = std::this_thread::get_id();
        ServiceThread* available = nullptr;
        for (auto& entry : instance.service_threads) {
            if (entry.count != 0 && entry.id == thread) {
                available = &entry;
                break;
            }
            if (entry.count == 0 && available == nullptr) {
                available = &entry;
            }
        }
        if (available == nullptr ||
            instance.active_calls == std::numeric_limits<std::size_t>::max() ||
            available->count == std::numeric_limits<std::size_t>::max()) {
            return RODAQ_STATUS_RESOURCE_EXHAUSTED;
        }
        available->id = thread;
        ++available->count;
        ++instance.active_calls;
        return RODAQ_STATUS_OK;
    } catch (...) {
        instance.manager_identity->call_violation.store(true);
        return RODAQ_STATUS_INTERNAL;
    }
}

void leave_service(rodaq_service_registry_context_t* context) noexcept {
    if (context == nullptr) {
        return;
    }
    auto& instance = *reinterpret_cast<InstanceServices*>(context)->instance;
    try {
        const auto owner = instance.shared_from_this();
        {
            std::lock_guard<std::mutex> lock(instance.mutex);
            ServiceThread* selected = nullptr;
            const auto thread = std::this_thread::get_id();
            for (auto& entry : instance.service_threads) {
                if (entry.count != 0 && entry.id == thread) {
                    selected = &entry;
                    break;
                }
                if (entry.count != 0 && selected == nullptr) {
                    selected = &entry;
                }
            }
            // Raw permits may transfer threads. They are interchangeable within
            // one Registry, but must never consume an Operation's call count.
            if (selected == nullptr || instance.active_calls == 0) {
                instance.manager_identity->call_violation.store(true);
                return;
            }
            if (--selected->count == 0) {
                selected->id = {};
            }
            --instance.active_calls;
        }
        instance.changed.notify_all();
    } catch (...) {
        instance.manager_identity->call_violation.store(true);
    }
}

const rodaq_service_registry_vtable_t kRegistryVtable{
    {sizeof(rodaq_service_registry_vtable_t)}, query_service, enter_service, leave_service};

}  // namespace

std::int64_t monotonic_now() noexcept {
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0 ||
        value.tv_nsec >= 1000000000) {
        return -1;
    }
    constexpr auto kLatest = std::numeric_limits<std::int64_t>::max() - 1;
    if (value.tv_sec > kLatest / 1000000000 ||
        (value.tv_sec == kLatest / 1000000000 && value.tv_nsec > kLatest % 1000000000)) {
        return kLatest;
    }
    return static_cast<std::int64_t>(value.tv_sec) * 1000000000 + value.tv_nsec;
}

InstanceState::InstanceState(
    const std::shared_ptr<ManagerState>& owner, std::shared_ptr<LibraryState> provider,
    std::shared_ptr<RegistryState> registry, std::string configuration, std::size_t capacity_slot,
    PluginInstanceId identity)
: manager(owner),
  manager_identity(owner.get()),
  library(std::move(provider)),
  services{this, std::move(registry)},
  native_services{reinterpret_cast<rodaq_service_registry_context_t*>(&services), &kRegistryVtable},
  config(std::move(configuration)),
  slot(capacity_slot),
  id(identity) {}

LifecycleInvocation::LifecycleInvocation(ManagerState& manager, InstanceState* instance) noexcept
: previous_manager_(lifecycle_manager), previous_instance_(lifecycle_instance) {
    lifecycle_manager = &manager;
    lifecycle_instance = instance;
}

LifecycleInvocation::~LifecycleInvocation() {
    lifecycle_manager = previous_manager_;
    lifecycle_instance = previous_instance_;
}

bool current_invocation(const InstanceState& instance) noexcept {
    if (lifecycle_instance == &instance) {
        return true;
    }
    for (std::size_t i = 0; i < current_call_count; ++i) {
        if (current_calls[i] == &instance) {
            return true;
        }
    }
    return false;
}

bool current_manager_invocation(const ManagerState& manager) noexcept {
    if (lifecycle_manager == &manager) {
        return true;
    }
    for (std::size_t i = 0; i < current_call_count; ++i) {
        if (current_calls[i]->manager_identity == &manager) {
            return true;
        }
    }
    return false;
}

bool current_service_locked(const InstanceState& instance) noexcept {
    for (const auto& entry : instance.service_threads) {
        if (entry.count != 0 && entry.id == std::this_thread::get_id()) {
            return true;
        }
    }
    return false;
}

namespace invocation {

CallGuard::CallGuard(std::shared_ptr<InstanceState> state) noexcept : state_(std::move(state)) {}

CallGuard::CallGuard(CallGuard&&) noexcept = default;

CallGuard::~CallGuard() noexcept {
    if (!state_) {
        return;
    }
    for (std::size_t i = current_call_count; i != 0; --i) {
        if (current_calls[i - 1] == state_.get()) {
            for (std::size_t j = i; j < current_call_count; ++j) {
                current_calls[j - 1] = current_calls[j];
            }
            --current_call_count;
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        assert(state_->active_calls != 0);
        --state_->active_calls;
    }
    state_->changed.notify_all();
}

Result<CallGuard> acquire(const std::shared_ptr<InstanceState>& state) {
    if (!state) {
        return Status(StatusCode::InvalidState, "Empty Operation owner");
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->phase != PluginInstancePhase::Open) {
        return Status(StatusCode::InvalidState, "Plugin Instance is not Open");
    }
    if (current_call_count == current_calls.size() ||
        state->active_calls == std::numeric_limits<std::size_t>::max()) {
        return Status(StatusCode::ResourceExhausted, "Managed call nesting limit");
    }
    ++state->active_calls;
    current_calls[current_call_count++] = state.get();
    return CallGuard(state);
}

Result<rodaq_interface_reference_t> query_native(
    const std::shared_ptr<InstanceState>& state, const rodaq_interface_query_t& query) {
    std::array<std::uint8_t, 512> buffer{};
    rodaq_error_t error{{buffer.data(), buffer.size(), 0}};
    const abi::BufferSnapshot expected{buffer.data(), buffer.size()};
    rodaq_interface_reference_t reference{};
    rodaq_status_code_t code = RODAQ_STATUS_INTERNAL;
    try {
        code = state->functions.query(state->native.context, &query, &reference, &error);
    } catch (...) {
        enqueue_diagnostic(
            state->manager.lock(), "Exception crossed Plugin query ABI", &state->library->info,
            state->id, "query");
        return Status(StatusCode::Internal, "Exception crossed Plugin query ABI");
    }
    if (!abi::known_status(code) || !abi::valid_error(code, error, expected) ||
        (code != RODAQ_STATUS_OK && !abi::default_invalid(reference))) {
        enqueue_diagnostic(
            state->manager.lock(), "Plugin query output contract violation", &state->library->info,
            state->id, "query");
    }
    auto status = abi::status_from_abi(code, error, expected);
    if (!status.ok()) {
        return status;
    }
    return reference;
}

}  // namespace invocation

PluginInstance::PluginInstance(std::shared_ptr<InstanceState> state) noexcept
: state_(std::move(state)) {}

PluginInstance::PluginInstance(PluginInstance&&) noexcept = default;

PluginInstance::~PluginInstance() noexcept {
    if (state_) {
        async_close();
    }
}

PluginInstance::operator bool() const noexcept { return static_cast<bool>(state_); }

PluginInstancePhase PluginInstance::phase() const noexcept {
    assert(state_);
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->phase;
}

PluginInstanceId PluginInstance::id() const noexcept {
    assert(state_);
    return state_->id;
}

CloseFuture PluginInstance::async_close() noexcept {
    assert(state_);
    if (auto manager = state_->manager.lock()) {
        {
            std::lock_guard<std::mutex> lock(manager->mutex);
            request_close_locked(*state_);
        }
        manager->changed.notify_all();
    }
    return CloseFuture(state_);
}

Result<void> PluginInstance::close(Deadline deadline) {
    assert(state_);
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (current_invocation(*state_) || current_service_locked(*state_)) {
            return Status(StatusCode::InvalidState, "Close would await current invocation");
        }
    }
    const auto future = async_close();
    if (future.wait(deadline) == CloseWaitStatus::TimedOut) {
        return Status(StatusCode::DeadlineExceeded, "Close observer timed out");
    }
    return future.result();
}

CloseFuture::CloseFuture(std::shared_ptr<InstanceState> state) noexcept
: state_(std::move(state)) {}

CloseFuture::CloseFuture(CloseFuture&&) noexcept = default;

CloseFuture& CloseFuture::operator=(CloseFuture&&) noexcept = default;

CloseFuture::operator bool() const noexcept { return static_cast<bool>(state_); }

bool CloseFuture::ready() const noexcept {
    assert(state_);
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->terminal;
}

CloseWaitStatus CloseFuture::wait(Deadline deadline) const noexcept {
    assert(state_);
    try {
        std::unique_lock<std::mutex> lock(state_->mutex);
        while (!state_->terminal) {
            if (current_invocation(*state_) || current_service_locked(*state_)) {
                return CloseWaitStatus::TimedOut;
            }
            if (deadline.is_infinite()) {
                state_->changed.wait(lock);
            } else {
                const auto now = monotonic_now();
                if (now < 0 || now >= deadline.native().monotonic_deadline) {
                    return CloseWaitStatus::TimedOut;
                }
                state_->changed.wait_for(
                    lock, std::chrono::nanoseconds(deadline.native().monotonic_deadline - now));
            }
        }
        return CloseWaitStatus::Completed;
    } catch (...) {
        if (auto manager = state_->manager.lock()) {
            manager->call_violation.store(true);
        }
        return CloseWaitStatus::TimedOut;
    }
}

Result<void> CloseFuture::result() const {
    assert(state_);
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->terminal) {
        return Status(StatusCode::InvalidState, "Close result is not ready");
    }
    return state_->close_result.ok() ? Result<void>{} : Result<void>{state_->close_result};
}

}  // namespace rodaq::plugin
