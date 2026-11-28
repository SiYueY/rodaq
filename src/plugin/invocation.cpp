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

#include <rodaq/logging.hpp>

namespace rodaq::plugin {
namespace {

thread_local std::array<InstanceState*, 64> current_calls{};
thread_local std::size_t current_call_count = 0;
thread_local ManagerState* lifecycle_manager = nullptr;
thread_local InstanceState* lifecycle_instance = nullptr;

logging::Level to_logging_level(rodaq_log_level_t level) noexcept {
    switch (level) {
        case RODAQ_LOG_TRACE:
            return logging::Level::Trace;
        case RODAQ_LOG_DEBUG:
            return logging::Level::Debug;
        case RODAQ_LOG_INFO:
            return logging::Level::Info;
        case RODAQ_LOG_WARNING:
            return logging::Level::Warning;
        case RODAQ_LOG_ERROR:
            return logging::Level::Error;
        case RODAQ_LOG_CRITICAL:
            return logging::Level::Critical;
        default:
            return logging::Level::Warning;
    }
}

void application_log_callback(
    void* /*context*/, rodaq_log_level_t level, rodaq_string_t message) noexcept {
    if (message.size == 0 || message.data == nullptr) {
        return;
    }
    constexpr std::size_t kMaxPluginLogBytes = 4096;
    const auto bounded = abi::utf8_prefix({message.data, message.size}, kMaxPluginLogBytes);
    if (bounded == 0) {
        return;
    }
    logging::write(to_logging_level(level), {}, {message.data, bounded});
}

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
    std::string configuration, std::size_t capacity_slot, PluginInstanceId identity)
: manager(owner),
  manager_identity(owner.get()),
  library(std::move(provider)),
  application_callbacks{{sizeof(rodaq_application_callbacks_t)}, nullptr, application_log_callback},
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

void report_abi_violation(
    const std::shared_ptr<InstanceState>& state, std::string_view message) noexcept {
    if (state) {
        enqueue_diagnostic(
            state->manager.lock(), message, &state->library->info, state->id, "operation");
    }
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
        if (current_invocation(*state_)) {
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
            if (current_invocation(*state_)) {
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
