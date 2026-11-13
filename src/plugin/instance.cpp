#include "plugin_state.hpp"

#include "abi_support.hpp"

#include <rodaq/plugin/deadline.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <new>
#include <utility>

namespace rodaq::plugin {
namespace {

std::chrono::steady_clock::time_point time_from_deadline(Deadline deadline) {
    return std::chrono::steady_clock::time_point{DeadlineDuration{deadline.nanoseconds()}};
}

rodaq_error_t make_error_buffer(
    std::array<std::uint8_t, interface_support::kErrorMessageCapacity>& storage) noexcept {
    rodaq_error_t error{};
    error.message.data = storage.data();
    error.message.capacity = static_cast<std::uint32_t>(storage.size());
    return error;
}

Result<void> acquire_operation(void* owner) noexcept {
    return static_cast<PluginInstanceState*>(owner)->acquire();
}

void release_operation(void* owner) noexcept {
    static_cast<PluginInstanceState*>(owner)->release();
}

void report_contract_violation(void* owner, StatusCode code, std::string_view message) noexcept {
    static_cast<PluginInstanceState*>(owner)->record_contract_violation(code, message);
}

}  // namespace

PluginInstanceState::PluginInstanceState(
    PluginInstanceId id, std::shared_ptr<PluginLibraryState> library,
    std::shared_ptr<const ServiceRegistryState> services, std::weak_ptr<PluginManagerState> manager)
: id_(id),
  library_(std::move(library)),
  services_(std::move(services)),
  manager_(std::move(manager)) {}

Result<void> PluginInstanceState::acquire() noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    if (phase_ != PluginInstancePhase::Open) {
        return Status{StatusCode::InvalidState, "plugin instance is not open"};
    }
    ++active_call_count_;
    return {};
}

void PluginInstanceState::release() noexcept {
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (active_call_count_ > 0u) {
            --active_call_count_;
        }
    }
    condition_.notify_all();
}

PluginInstancePhase PluginInstanceState::phase() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return phase_;
}

void PluginInstanceState::record_contract_violation(
    StatusCode code, std::string_view message) noexcept {
    if (auto manager = manager_.lock()) {
        manager->mark_degraded(code, message);
    }
}

Result<PluginInstance::QueryAdmission> PluginInstanceState::query(
    std::string_view interface_id, std::uint16_t major, std::uint16_t min_minor,
    std::uint16_t max_minor) {
    auto acquired = acquire();
    if (!acquired.ok()) {
        return acquired.status();
    }

    struct Release final {
        PluginInstanceState* state;
        bool active = true;

        void dismiss() noexcept { active = false; }
        ~Release() {
            if (active) {
                state->release();
            }
        }
    } release_guard{this, true};

    const auto raw = raw_instance();
    if (raw.context == nullptr || raw.vtable == nullptr || raw.vtable->query == nullptr) {
        return Status{StatusCode::InvalidState, "plugin instance vtable is incomplete"};
    }

    rodaq_interface_query_t query{};
    query.interface_id = string_to_abi(interface_id);
    query.major = major;
    query.min_minor = min_minor;
    query.max_minor = max_minor;

    rodaq_interface_reference_t reference{};
    std::array<std::uint8_t, interface_support::kErrorMessageCapacity> error_storage{};
    auto error = make_error_buffer(error_storage);
    Result<void> result;
    try {
        const auto code = raw.vtable->query(raw.context, &query, &reference, &error);
        result = result_from_abi_call(code, &error);
    } catch (const std::bad_alloc&) {
        result = Status{StatusCode::ResourceExhausted, "plugin query allocation failed"};
    } catch (...) {
        result = Status{StatusCode::Internal, "plugin query threw an exception"};
    }
    if (!result.ok()) {
        return result.status();
    }

    auto managed = PluginInstance::QueryAdmission{
        std::shared_ptr<void>{shared_from_this()}, reference, &acquire_operation,
        &release_operation, &report_contract_violation};
    release_guard.dismiss();
    return managed;
}

PluginInstance::QueryAdmission::QueryAdmission(
    std::shared_ptr<void> owner, rodaq_interface_reference_t reference,
    Result<void> (*acquire)(void*) noexcept, void (*release)(void*) noexcept,
    void (*report_violation)(void*, StatusCode, std::string_view) noexcept) noexcept
: owner_(std::move(owner)),
  reference_(reference),
  acquire_(acquire),
  release_(release),
  report_violation_(report_violation),
  admission_held_(true) {}

PluginInstance::QueryAdmission::QueryAdmission(QueryAdmission&& other) noexcept
: owner_(std::move(other.owner_)),
  reference_(other.reference_),
  acquire_(other.acquire_),
  release_(other.release_),
  report_violation_(other.report_violation_),
  admission_held_(other.admission_held_) {
    other.admission_held_ = false;
}

PluginInstance::QueryAdmission& PluginInstance::QueryAdmission::operator=(
    QueryAdmission&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release_admission();
    owner_ = std::move(other.owner_);
    reference_ = other.reference_;
    acquire_ = other.acquire_;
    release_ = other.release_;
    report_violation_ = other.report_violation_;
    admission_held_ = other.admission_held_;
    other.admission_held_ = false;
    return *this;
}

PluginInstance::QueryAdmission::~QueryAdmission() { release_admission(); }

void PluginInstance::QueryAdmission::release_admission() noexcept {
    if (admission_held_ && owner_ != nullptr && release_ != nullptr) {
        admission_held_ = false;
        release_(owner_.get());
        return;
    }
    admission_held_ = false;
}

CloseFuture PluginInstanceState::request_close() noexcept {
    auto manager = manager_.lock();
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (phase_ == PluginInstancePhase::Open || phase_ == PluginInstancePhase::Initializing) {
            phase_ = PluginInstancePhase::Closing;
            close_ready_ = false;
            close_stage_ = CloseStage::None;
            close_stage_started_ = std::chrono::steady_clock::now();
        }
    }
    if (manager != nullptr) {
        manager->request_close(shared_from_this());
    }
    return CloseFuture{shared_from_this()};
}

bool PluginInstanceState::open_for_use() noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    if (phase_ != PluginInstancePhase::Initializing) {
        return false;
    }
    phase_ = PluginInstancePhase::Open;
    return true;
}

bool PluginInstanceState::try_begin_close() noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    if (phase_ != PluginInstancePhase::Closing || close_worker_active_) {
        return false;
    }
    close_worker_active_ = true;
    return true;
}

std::chrono::steady_clock::time_point PluginInstanceState::close_stage_started() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return close_stage_started_;
}

CloseStage PluginInstanceState::close_stage() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return close_stage_;
}

void PluginInstanceState::set_close_stage(CloseStage stage) noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    close_stage_ = stage;
    close_stage_started_ = std::chrono::steady_clock::now();
}

void PluginInstanceState::complete_close(Result<void> result) noexcept {
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (close_ready_) {
            return;
        }
        phase_ = PluginInstancePhase::Closed;
        close_stage_ = CloseStage::TerminalPublication;
        close_result_ = std::move(result);
        close_ready_ = true;
        raw_instance_ = rodaq_plugin_instance_t{};
    }
    condition_.notify_all();
}

void PluginInstanceState::mark_unresolved(Result<void> result) noexcept {
    {
        std::lock_guard<std::mutex> lock{mutex_};
        phase_ = PluginInstancePhase::Unresolved;
        close_stage_ = CloseStage::TerminalPublication;
        close_result_ = std::move(result);
        close_ready_ = true;
    }
    condition_.notify_all();
}

void PluginInstanceState::mark_initialization_closed() noexcept {
    {
        std::lock_guard<std::mutex> lock{mutex_};
        phase_ = PluginInstancePhase::Closed;
        close_ready_ = true;
        close_result_ = {};
        raw_instance_ = rodaq_plugin_instance_t{};
        close_stage_ = CloseStage::TerminalPublication;
    }
    condition_.notify_all();
}

bool PluginInstanceState::close_ready() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return close_ready_;
}

CloseWaitStatus PluginInstanceState::wait(Deadline deadline) const noexcept {
    std::unique_lock<std::mutex> lock{mutex_};
    if (close_ready_) {
        return CloseWaitStatus::Completed;
    }
    if (deadline.is_infinite()) {
        condition_.wait(lock, [this] { return close_ready_; });
        return CloseWaitStatus::Completed;
    }
    return condition_.wait_until(
               lock, time_from_deadline(deadline), [this] { return close_ready_; })
               ? CloseWaitStatus::Completed
               : CloseWaitStatus::TimedOut;
}

Result<void> PluginInstanceState::close_result() const {
    std::lock_guard<std::mutex> lock{mutex_};
    if (!close_ready_) {
        return Status{StatusCode::InvalidState, "plugin close is not terminal"};
    }
    return close_result_;
}

void PluginInstanceState::run_close(
    DeadlineDuration native_timeout, std::weak_ptr<PluginManagerState> manager) {
    set_close_stage(CloseStage::NativeShutdown);

    const auto raw = raw_instance();
    std::array<std::uint8_t, interface_support::kErrorMessageCapacity> error_storage{};
    auto error = make_error_buffer(error_storage);
    const auto native_deadline = to_abi_deadline(Deadline::after(native_timeout));
    const auto shutdown_code = raw.vtable != nullptr && raw.vtable->shutdown != nullptr
                                   ? raw.vtable->shutdown(raw.context, native_deadline, &error)
                                   : static_cast<rodaq_status_code_t>(RODAQ_STATUS_INVALID_STATE);
    auto shutdown_result = result_from_abi_call(shutdown_code, &error);
    if (!shutdown_result.ok()) {
        auto failure = std::move(shutdown_result).status();
        if (auto state = manager.lock()) {
            state->mark_degraded(
                failure.code(), "plugin shutdown did not establish a safe destroy state");
        }
        mark_unresolved(std::move(failure));
        return;
    }

    set_close_stage(CloseStage::CallDrain);
    {
        std::unique_lock<std::mutex> lock{mutex_};
        condition_.wait(lock, [this] { return active_call_count_ == 0u; });
    }

    set_close_stage(CloseStage::Destroy);
    if (raw.vtable != nullptr && raw.vtable->destroy != nullptr) {
        raw.vtable->destroy(raw.context);
    }
    {
        std::lock_guard<std::mutex> lock{mutex_};
        raw_instance_ = rodaq_plugin_instance_t{};
    }

    set_close_stage(CloseStage::BackingRelease);
    std::shared_ptr<const ServiceRegistryState> detached_services;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        detached_services = std::move(services_);
    }
    detached_services.reset();

    if (auto state = manager.lock()) {
        state->finish_closed_instance(id_);
    }
    complete_close({});
}

std::int64_t PluginInstanceState::now_nanoseconds() const noexcept {
    return std::chrono::duration_cast<DeadlineDuration>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

CloseFuture::CloseFuture(std::shared_ptr<PluginInstanceState> state) noexcept
: state_(std::move(state)) {}

CloseFuture::CloseFuture(CloseFuture&&) noexcept = default;
CloseFuture& CloseFuture::operator=(CloseFuture&&) noexcept = default;

CloseFuture::operator bool() const noexcept { return state_ != nullptr; }

bool CloseFuture::ready() const noexcept { return state_ != nullptr && state_->close_ready(); }

CloseWaitStatus CloseFuture::wait(Deadline deadline) const noexcept {
    if (state_ == nullptr || interface_support::in_active_operation(state_.get())) {
        return CloseWaitStatus::TimedOut;
    }
    return state_->wait(deadline);
}

Result<void> CloseFuture::result() const {
    if (state_ == nullptr) {
        return Status{StatusCode::InvalidState, "close future is empty"};
    }
    return state_->close_result();
}

PluginInstance::PluginInstance(std::shared_ptr<PluginInstanceState> state) noexcept
: state_(std::move(state)) {}

PluginInstance::PluginInstance(PluginInstance&&) noexcept = default;

PluginInstance::~PluginInstance() noexcept {
    if (state_ != nullptr && state_->phase() == PluginInstancePhase::Open) {
        (void)state_->request_close();
    }
}

PluginInstance::operator bool() const noexcept { return state_ != nullptr; }

Result<void> PluginInstance::close(Deadline deadline) {
    if (state_ != nullptr && interface_support::in_active_operation(state_.get())) {
        return Status{
            StatusCode::InvalidState, "cannot synchronously close from its own operation"};
    }
    auto future = async_close();
    return future.wait(deadline) == CloseWaitStatus::Completed
               ? future.result()
               : Result<void>{Status{StatusCode::DeadlineExceeded, "plugin close wait timed out"}};
}

CloseFuture PluginInstance::async_close() noexcept {
    if (state_ == nullptr) {
        return CloseFuture{std::shared_ptr<PluginInstanceState>{}};
    }
    return state_->request_close();
}

PluginInstancePhase PluginInstance::phase() const noexcept {
    return state_ == nullptr ? PluginInstancePhase::Closed : state_->phase();
}

PluginInstanceId PluginInstance::id() const noexcept {
    return state_ == nullptr ? 0u : state_->id();
}

Result<PluginInstance::QueryAdmission> PluginInstance::query_managed(
    std::string_view interface_id, std::uint16_t major, std::uint16_t min_minor,
    std::uint16_t max_minor) {
    if (state_ == nullptr) {
        return Status{StatusCode::InvalidState, "plugin instance is empty"};
    }
    return state_->query(interface_id, major, min_minor, max_minor);
}

}  // namespace rodaq::plugin
