#include "runtime/runtime.hpp"

#include "plugin/acquisition_plugin_instance.hpp"
#include "plugin/plugin_manager.hpp"
#include "plugin/service_registry.hpp"
#include "plugin/storage_plugin_instance.hpp"
#include "runtime/active_runtime_state.hpp"
#include "runtime/quarantine.hpp"

#include <rodaq/plugin/abi/acquisition.h>
#include <rodaq/plugin/abi_support.hpp>
#include <rodaq/plugin/abi/storage.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rodaq::runtime {

namespace {

using Clock = std::chrono::steady_clock;

Status runtime_status(StatusCode status_code, std::string message) {
    return Status(status_code, "runtime", 0, std::move(message));
}

template <typename Config>
Result<void> validate_plugin_options(const Config& options) {
    std::error_code error;
    if (options.path.empty() || !std::filesystem::is_regular_file(options.path, error)) {
        return runtime_status(StatusCode::NotFound, "Runtime Plugin path is missing");
    }
    const ConfigNodeView view = options.config.view();
    if (view.abi_view().context == nullptr) {
        return runtime_status(StatusCode::InvalidArgument, "Runtime Plugin config is empty");
    }
    return {};
}

Result<void> validate_options(const RuntimeConfig& options) {
    const ResourceConfig& resources = options.resources;
    const PayloadPoolConfig& pool_config = resources.payload_pool;
    if (resources.max_sources == 0 || resources.max_clocks == 0 || resources.max_schemas == 0 ||
        resources.max_timestamp_key_sets == 0 || resources.max_streams == 0 ||
        resources.max_stream_descriptors == 0 || resources.max_bindings == 0 ||
        resources.structural_arena_bytes == 0 || resources.max_single_payload_bytes == 0 ||
        resources.dispatch_capacity == 0 || resources.operation_capacity == 0 ||
        resources.completion_capacity == 0 || resources.payload_release_workers == 0 ||
        options.submit_wait_timeout.count() <= 0 || options.stop_progress_timeout.count() <= 0 ||
        options.logging.queue_records == 0 || options.logging.queue_bytes == 0 ||
        options.logging.flush_timeout.count() <= 0 ||
        options.logging.shutdown_timeout.count() <= 0) {
        return runtime_status(
            StatusCode::InvalidArgument, "Runtime capacity configuration is invalid");
    }
    if (resources.dispatch_capacity != resources.operation_capacity) {
        return runtime_status(
            StatusCode::InvalidArgument, "V1 requires equal dispatch and operation capacity");
    }
    if (pool_config.classes.empty()) {
        return runtime_status(
            StatusCode::InvalidArgument, "PayloadPool configuration has no size classes");
    }
    std::size_t previous_class_size = 0;
    for (const PayloadClassConfig& payload_class : pool_config.classes) {
        if (payload_class.max_payload_bytes == 0 || payload_class.block_count == 0 ||
            payload_class.max_payload_bytes <= previous_class_size ||
            payload_class.max_payload_bytes >
                std::numeric_limits<std::size_t>::max() / payload_class.block_count) {
            return runtime_status(
                StatusCode::InvalidArgument, "PayloadPool size class configuration is invalid");
        }
        previous_class_size = payload_class.max_payload_bytes;
    }
    if (pool_config.classes.back().max_payload_bytes < resources.max_single_payload_bytes) {
        return runtime_status(
            StatusCode::InvalidArgument,
            "PayloadPool capacity is below the configured payload limit");
    }
    if (options.session_id.empty() || options.target_uri.empty()) {
        return runtime_status(StatusCode::InvalidArgument, "Storage open configuration is empty");
    }
    if (options.acquisitions.empty()) {
        return runtime_status(
            StatusCode::InvalidArgument, "At least one Acquisition Plugin is required");
    }
    auto storage = validate_plugin_options(options.storage);
    if (!storage.ok()) {
        return storage;
    }
    for (const PluginConfig& acquisition : options.acquisitions) {
        auto validated = validate_plugin_options(acquisition);
        if (!validated.ok()) {
            return validated;
        }
    }
    return {};
}

Status abi_failure_status(
    rodaq_status_code status, const rodaq_status& error, std::string_view fallback) {
    auto converted = status_from_abi(status, &error, "runtime");
    if (!converted.ok()) {
        return converted;
    }
    return runtime_status(StatusCode::InternalError, std::string(fallback));
}

std::size_t saturating_product(std::size_t lhs, std::size_t rhs) noexcept {
    return lhs > std::numeric_limits<std::size_t>::max() / rhs
               ? std::numeric_limits<std::size_t>::max()
               : lhs * rhs;
}

std::size_t bounded_size(std::uint64_t value) noexcept {
    return value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
               ? std::numeric_limits<std::size_t>::max()
               : static_cast<std::size_t>(value);
}

std::chrono::nanoseconds effective_control_timeout(
    std::chrono::nanoseconds runtime_timeout, std::uint64_t backend_timeout_ns) noexcept {
    if (backend_timeout_ns == 0) {
        return runtime_timeout;
    }
    return std::min(
        runtime_timeout, std::chrono::nanoseconds(static_cast<std::int64_t>(std::min<std::uint64_t>(
                             backend_timeout_ns, static_cast<std::uint64_t>(
                                                     std::numeric_limits<std::int64_t>::max())))));
}

struct BoundedCallOutcome {
    enum class Outcome {
        Completed,
        Failed,
        TimedOut,
    };

    Outcome status = Outcome::Failed;
    Status error;
};

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

template <typename Callable>
BoundedCallOutcome supervise_call(
    Callable callable, std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<std::atomic<bool>> joined = {}) noexcept {
    auto managed = QuarantineManager::process().run(
        std::function<Result<void>()>(std::move(callable)), deadline, "Runtime Plugin control call",
        std::move(joined));
    if (!managed.ok()) {
        return BoundedCallOutcome{BoundedCallOutcome::Outcome::Failed, managed.status()};
    }
    switch (managed.value().status) {
        case SupervisedCallStatus::Completed:
            return BoundedCallOutcome{BoundedCallOutcome::Outcome::Completed, {}};
        case SupervisedCallStatus::Failed:
            return BoundedCallOutcome{
                BoundedCallOutcome::Outcome::Failed, std::move(managed.value().error)};
        case SupervisedCallStatus::TimedOut:
            return BoundedCallOutcome{BoundedCallOutcome::Outcome::TimedOut, {}};
    }
    return BoundedCallOutcome{
        BoundedCallOutcome::Outcome::Failed,
        runtime_status(StatusCode::InternalError, "Unknown supervised call outcome")};
}

}  // namespace

class Runtime::Impl final {
    // Ownership is logical: readers may acquire the mutex while Plugin I/O
    // runs, but another lifecycle operation must wait for this owner.
    class LifecycleOwner final {
    public:
        explicit LifecycleOwner(Impl& owner) : owner_(owner), lock_(owner.lifecycle_mutex_) {
            owner_.lifecycle_condition_.wait(lock_, [&] { return !owner_.lifecycle_owned_; });
            owner_.lifecycle_owned_ = true;
        }
        ~LifecycleOwner() {
            owner_.lifecycle_owned_ = false;
            owner_.lifecycle_condition_.notify_all();
        }

    private:
        Impl& owner_;
        std::unique_lock<std::mutex> lock_;
    };

public:
    explicit Impl(RuntimeConfig config)
    : options_(std::move(config)),
      acquisition_states_(options_.acquisitions.size(), AcquisitionActivationState::Empty) {}

    Result<void> configure() {
        LifecycleOwner owner(*this);
        if (state_ != RuntimeState::Constructed) {
            return runtime_status(
                StatusCode::InvalidState, "Runtime cannot be configured from its state");
        }
        auto validated = validate_options(options_);
        if (!validated.ok()) {
            return validated;
        }
        state_ = RuntimeState::Configured;
        return {};
    }

    Result<void> start() {
        LifecycleOwner owner(*this);
        if (state_ != RuntimeState::Configured && state_ != RuntimeState::Stopped) {
            return runtime_status(StatusCode::InvalidState, "Runtime cannot start from its state");
        }
        if (quarantined_ != nullptr) {
            return runtime_status(
                StatusCode::Unavailable,
                "Runtime restart is blocked by quarantined callback state");
        }
        if (activation_generation_ == std::numeric_limits<std::uint32_t>::max()) {
            return runtime_status(
                StatusCode::ResourceExhausted, "Runtime activation generation is exhausted");
        }
        auto activation_slot =
            QuarantineManager::process().reserve_activation("Runtime activation");
        if (!activation_slot.ok()) {
            return std::move(activation_slot).status();
        }
        activation_slot_ = activation_slot.value();
        std::fill(
            acquisition_states_.begin(), acquisition_states_.end(),
            AcquisitionActivationState::Empty);
        storage_state_ = StorageActivationState::Empty;
        storage_flushed_ = false;
        aborting_ = false;
        ++activation_generation_;
        state_ = RuntimeState::Starting;
        auto created = ActiveRuntimeState::create(options_, activation_generation_);
        if (!created.ok()) {
            release_activation_slot();
            state_ = RuntimeState::Failed;
            return std::move(created).status();
        }
        active_ = std::move(created.value());
        active_->set_failure_signal(failure_signal_);
        if (!failure_worker_.joinable()) {
            try {
                failure_worker_ = std::thread([this] { failure_loop(); });
            } catch (const std::exception& error) {
                active_->request_failure(runtime_status(StatusCode::Unavailable, error.what()));
                abort_start();
                return runtime_status(StatusCode::Unavailable, error.what());
            }
        }

        auto descriptor_worker = active_->start_descriptor_reconciliation();
        if (!descriptor_worker.ok()) {
            active_->request_failure(descriptor_worker.status());
            abort_start();
            state_ = RuntimeState::Failed;
            return descriptor_worker;
        }
        auto started = start_activation();
        if (!started.ok()) {
            if (!active_->has_primary_failure()) {
                active_->request_failure(started.status());
            }
            abort_start();
            state_ = RuntimeState::Failed;
            return started;
        }
        if (active_->has_primary_failure()) {
            auto error = active_->primary_failure();
            abort_start();
            return error;
        }
        if (!active_->mark_running()) {
            auto error = active_->primary_failure();
            abort_start();
            return error;
        }
        state_ = RuntimeState::Running;
        return {};
    }

    Result<void> stop() {
        LifecycleOwner owner(*this);
        return stop_internal();
    }

private:
    Result<void> stop_internal() {
        if (state_ == RuntimeState::Stopped) {
            return {};
        }
        if (state_ != RuntimeState::Running && state_ != RuntimeState::Failed &&
            state_ != RuntimeState::Starting && state_ != RuntimeState::Stopping) {
            return runtime_status(StatusCode::InvalidState, "Runtime cannot stop from its state");
        }
        if (active_ == nullptr) {
            if (quarantined_ != nullptr || state_ == RuntimeState::Failed) {
                return runtime_status(StatusCode::Unavailable, "Runtime stop requires quarantine");
            }
            state_ = RuntimeState::Stopped;
            return {};
        }

        state_ = RuntimeState::Stopping;
        active_->mark_stopping();
        if (active_->has_primary_failure() && active_->pipeline() != nullptr) {
            active_->pipeline()->begin_failure_cutoff();
        }
        if (!outside_lifecycle([&] {
                return active_->close_and_join_descriptor_reconciliation(
                    Clock::now() + options_.stop_progress_timeout);
            })) {
            return quarantine_failure();
        }
        if (active_->requires_quarantine()) {
            return quarantine_failure();
        }

        for (std::size_t index = 0; index < acquisition_states_.size(); ++index) {
            if (acquisition_states_[index] != AcquisitionActivationState::Started ||
                !active_->acquisition_instance(index).has_value()) {
                continue;
            }
            std::shared_ptr<ActiveRuntimeState> state = active_;
            auto outcome = run_bounded_call(
                "quiesce",
                [state, index]() -> Result<void> {
                    auto control = state->acquisition_instance(index)->required_control_interface();
                    if (!control.ok()) {
                        return std::move(control).status();
                    }
                    const auto* table =
                        static_cast<const rodaq_acquisition_control_v1*>(control->vtable);
                    rodaq_status error{};
                    const rodaq_status_code status =
                        table->quiesce_plugin(control->context, &error);
                    if (status != RODAQ_STATUS_OK) {
                        return abi_failure_status(status, error, "Acquisition quiescence failed");
                    }
                    return {};
                },
                Clock::now() + options_.stop_progress_timeout);
            if (outcome.status == BoundedCallOutcome::Outcome::TimedOut) {
                active_->request_failure(
                    runtime_status(StatusCode::Timeout, "Acquisition quiescence timed out"));
                return quarantine_failure();
            }
            if (outcome.status == BoundedCallOutcome::Outcome::Failed) {
                active_->request_failure(std::move(outcome.error));
                return quarantine_failure();
            }
            acquisition_states_[index] = AcquisitionActivationState::Quiesced;
        }

        if (!outside_lifecycle([&] {
                return active_->wait_for_submissions(Clock::now() + options_.stop_progress_timeout);
            })) {
            active_->request_failure(
                runtime_status(StatusCode::Timeout, "Runtime submission barrier timed out"));
            return quarantine_failure();
        }
        if (!outside_lifecycle([&] {
                return active_->wait_for_admission_drain(
                    Clock::now() + options_.stop_progress_timeout);
            })) {
            active_->request_failure(
                runtime_status(StatusCode::Timeout, "Runtime admission barrier timed out"));
            return quarantine_failure();
        }

        StoragePipeline* pipeline = active_->pipeline();
        if (pipeline != nullptr) {
            pipeline->close_dispatch();
            if (!outside_lifecycle([&] {
                    return pipeline->wait_dispatch_idle(
                        Clock::now() + options_.stop_progress_timeout);
                })) {
                active_->request_failure(
                    runtime_status(StatusCode::Timeout, "Runtime dispatch barrier timed out"));
                return quarantine_failure();
            }
            outside_lifecycle([&] { return pipeline->join_dispatch(); });
        }

        if (storage_open_ && !storage_flushed_ && active_->storage_instance().has_value()) {
            std::shared_ptr<ActiveRuntimeState> state = active_;
            auto flush = run_bounded_call(
                "flush", [state]() { return state->storage_instance()->flush(); },
                Clock::now() + effective_storage_flush_timeout_);
            if (flush.status == BoundedCallOutcome::Outcome::TimedOut) {
                active_->request_failure(
                    runtime_status(StatusCode::Timeout, "Storage flush timed out"));
                return quarantine_failure();
            }
            storage_flushed_ = true;
            if (flush.status == BoundedCallOutcome::Outcome::Failed) {
                active_->request_failure(std::move(flush.error));
            }
        }
        if (pipeline != nullptr && !outside_lifecycle([&] {
                return pipeline->wait_operations_idle(
                    Clock::now() + options_.stop_progress_timeout);
            })) {
            active_->request_failure(
                runtime_status(StatusCode::Timeout, "Runtime Storage operation barrier timed out"));
        }

        if (storage_open_ && active_->storage_instance().has_value()) {
            const rodaq_storage_close_disposition disposition =
                active_->state() == RuntimeState::Failed ? RODAQ_STORAGE_CLOSE_FAILURE
                                                         : RODAQ_STORAGE_CLOSE_NORMAL;
            std::shared_ptr<ActiveRuntimeState> state = active_;
            auto close = run_bounded_call(
                "close",
                [state, disposition]() {
                    rodaq_storage_close_request request{};
                    request.struct_size = sizeof(request);
                    request.disposition = disposition;
                    return state->storage_instance()->close(request);
                },
                Clock::now() + effective_storage_close_timeout_);
            if (close.status == BoundedCallOutcome::Outcome::TimedOut) {
                active_->request_failure(
                    runtime_status(StatusCode::Timeout, "Storage close timed out"));
                return quarantine_failure();
            }
            storage_open_ = false;
            storage_state_ = StorageActivationState::Closed;
            if (close.status == BoundedCallOutcome::Outcome::Failed) {
                active_->request_failure(std::move(close.error));
                storage_state_ = StorageActivationState::Quarantined;
                return quarantine_failure();
            }
        }
        if (pipeline != nullptr) {
            pipeline->resolve_after_close();
            pipeline->close_callbacks();
            outside_lifecycle([&] { return pipeline->join_completion(); });
            pipeline->close_release();
            const auto release_deadline = Clock::now() + options_.stop_progress_timeout;
            if (!outside_lifecycle([&] { return pipeline->wait_release_idle(release_deadline); }) ||
                !outside_lifecycle(
                    [&] { return pipeline->wait_operations_idle(release_deadline); })) {
                if (!pipeline->retain_blocked_release_workers()) {
                    active_->record_secondary_failure(runtime_status(
                        StatusCode::ResourceExhausted,
                        "Runtime release worker quarantine capacity is exhausted"));
                }
                active_->request_failure(runtime_status(
                    StatusCode::Timeout, "Runtime callback/release quiescence barrier timed out"));
                return quarantine_failure();
            }
            outside_lifecycle([&] { return pipeline->join_release(); });
        }

        const bool unresolved = pipeline != nullptr && pipeline->has_unresolved_operations();
        if (unresolved) {
            return quarantine_failure();
        }

        for (std::size_t index = 0; index < acquisition_states_.size(); ++index) {
            if (acquisition_states_[index] != AcquisitionActivationState::Quiesced ||
                !active_->acquisition_instance(index).has_value()) {
                continue;
            }
            std::shared_ptr<ActiveRuntimeState> state = active_;
            auto stopped = run_bounded_call(
                "stop", [state, index]() { return state->acquisition_instance(index)->stop(); },
                Clock::now() + options_.stop_progress_timeout);
            if (stopped.status == BoundedCallOutcome::Outcome::TimedOut) {
                active_->request_failure(
                    runtime_status(StatusCode::Timeout, "Acquisition stop timed out"));
                return quarantine_failure();
            }
            if (stopped.status == BoundedCallOutcome::Outcome::Failed) {
                active_->request_failure(std::move(stopped.error));
                return quarantine_failure();
            }
            acquisition_states_[index] = AcquisitionActivationState::Stopped;
        }

        for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
            if (!active_->acquisition_instance(index).has_value() ||
                !active_->acquisition_instance(index)->valid()) {
                continue;
            }
            std::shared_ptr<ActiveRuntimeState> state = active_;
            auto destroyed = run_bounded_call(
                "destroy-acquisition",
                [state, index]() {
                    state->acquisition_instance(index)->destroy();
                    return Result<void>();
                },
                Clock::now() + options_.stop_progress_timeout);
            if (destroyed.status == BoundedCallOutcome::Outcome::TimedOut) {
                active_->request_failure(
                    runtime_status(StatusCode::Timeout, "Acquisition destroy timed out"));
                return quarantine_failure();
            }
            if (destroyed.status == BoundedCallOutcome::Outcome::Failed) {
                active_->request_failure(std::move(destroyed.error));
                return quarantine_failure();
            }
        }
        if (active_->storage_instance().has_value() && active_->storage_instance()->valid()) {
            std::shared_ptr<ActiveRuntimeState> state = active_;
            auto destroyed = run_bounded_call(
                "destroy-storage",
                [state]() {
                    state->storage_instance()->destroy();
                    return Result<void>();
                },
                Clock::now() + effective_storage_close_timeout_);
            if (destroyed.status == BoundedCallOutcome::Outcome::TimedOut) {
                active_->request_failure(
                    runtime_status(StatusCode::Timeout, "Storage destroy timed out"));
                return quarantine_failure();
            }
            if (destroyed.status == BoundedCallOutcome::Outcome::Failed) {
                active_->request_failure(std::move(destroyed.error));
                return quarantine_failure();
            }
        }
        if (!outside_lifecycle([&] {
                return active_->shutdown_logging(Clock::now() + options_.logging.shutdown_timeout);
            })) {
            active_->quarantine_logging("Runtime logging shutdown timed out");
            active_->record_secondary_failure(
                runtime_status(StatusCode::Timeout, "Runtime logging shutdown timed out"));
        }

        const bool failed =
            active_->state() == RuntimeState::Failed || !active_->primary_failure().ok();
        active_->record_stop_latency();
        last_stats_ = active_->stats();
        auto payload_stats = active_->payload_pool_statistics();
        if (payload_stats.ok()) {
            last_payload_stats_ = std::move(payload_stats.value());
        }
        auto primary_failure = active_->primary_failure();
        if (!primary_failure.ok()) {
            auto secondary_failures = active_->secondary_failures();
            if (secondary_failures.ok()) {
                last_secondary_failures_ = std::move(secondary_failures.value());
            }
            last_secondary_failure_overflow_count_ = active_->secondary_failure_overflow_count();
            last_primary_failure_ = std::move(primary_failure);
        }

        for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
            active_->acquisition_instance(index).reset();
        }
        active_->storage_instance().reset();
        active_->mark_stopped();
        active_.reset();
        release_activation_slot();
        state_ = failed ? RuntimeState::Failed : RuntimeState::Stopped;
        if (failed) {
            state_ = RuntimeState::Failed;
        }
        if (failed) {
            return runtime_status(StatusCode::Unavailable, "Runtime stopped after failure");
        }
        return {};
    }

public:
    RuntimeState state() const noexcept {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ != nullptr && active_->state() == RuntimeState::Failed) {
            return RuntimeState::Failed;
        }
        return state_;
    }

    Result<rodaq_runtime_lifecycle_snapshot> lifecycle_snapshot() const {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ == nullptr) {
            const RuntimeState state = state_;
            rodaq_runtime_lifecycle_snapshot snapshot{};
            if (state == RuntimeState::Stopped) {
                snapshot.state = RODAQ_RUNTIME_STOPPED;
            } else if (state == RuntimeState::Failed) {
                snapshot.state = RODAQ_RUNTIME_FAILED;
            } else {
                snapshot.state = RODAQ_RUNTIME_CONFIGURED;
            }
            snapshot.generation = activation_generation_;
            return snapshot;
        }
        return active_->lifecycle_snapshot();
    }

    Status primary_failure() const {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ == nullptr) {
            if (quarantined_ != nullptr) {
                return quarantined_->primary_failure();
            }
            return last_primary_failure_;
        }
        return active_->primary_failure();
    }

    Result<std::vector<Status>> secondary_failures() const {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ != nullptr) {
            return active_->secondary_failures();
        }
        if (quarantined_ != nullptr) {
            return quarantined_->secondary_failures();
        }
        if (!last_primary_failure_.ok()) {
            return last_secondary_failures_;
        }
        return runtime_status(StatusCode::InvalidState, "Runtime has no failure history");
    }

    std::uint64_t secondary_failure_overflow_count() const noexcept {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ != nullptr) {
            return active_->secondary_failure_overflow_count();
        }
        if (quarantined_ != nullptr) {
            return quarantined_->secondary_failure_overflow_count();
        }
        return last_secondary_failure_overflow_count_;
    }

    Result<RuntimeStatsSnapshot> stats() const {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ != nullptr) {
            return active_->stats();
        }
        if (quarantined_ != nullptr) {
            return quarantined_->stats();
        }
        if (last_stats_.has_value()) {
            return last_stats_.value();
        }
        return runtime_status(StatusCode::InvalidState, "Runtime has no active statistics");
    }

    Result<PayloadPoolStats> payload_stats() const {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ != nullptr) {
            return active_->payload_pool_statistics();
        }
        if (quarantined_ != nullptr) {
            return quarantined_->payload_pool_statistics();
        }
        if (last_payload_stats_.has_value()) {
            return last_payload_stats_.value();
        }
        return runtime_status(StatusCode::InvalidState, "Runtime has no active Payload statistics");
    }

    ~Impl() {
        // Close the lifetime-safe mailbox before joining the sole supervisor.
        failure_signal_->close();
        if (failure_worker_.joinable()) {
            failure_worker_.join();
        }
        QuarantineManager::process().reap_completed();
        if (active_ != nullptr) {
            static_cast<void>(stop());
        }
        LifecycleOwner owner(*this);
        if (quarantined_ != nullptr) {
            if (activation_slot_.has_value()) {
                transfer_quarantine();
            }
            quarantined_.reset();
        } else {
            release_activation_slot();
        }
        QuarantineManager::process().reap_completed();
    }

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
        const char* site, Callable callable, Clock::time_point deadline) noexcept {
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
                            runtime_status(StatusCode::InternalError, "Plugin control call threw");
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

    void failure_loop() noexcept {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(failure_signal_->mutex);
                failure_signal_->condition.wait(
                    lock, [this] { return failure_signal_->requested || failure_signal_->closed; });
                if (failure_signal_->closed) {
                    return;
                }
                failure_signal_->requested = false;
            }
            LifecycleOwner owner(*this);
            // Start's abort and an already owning Stop finish their own cleanup.
            if (active_ != nullptr && active_->has_primary_failure()) {
                static_cast<void>(stop_internal());
            }
        }
    }

    template <typename Callable>
    Result<void> supervise_start_call(Callable callable) {
        auto outcome = run_bounded_call(
            "activation-call", std::move(callable), Clock::now() + options_.stop_progress_timeout);
        if (outcome.status == BoundedCallOutcome::Outcome::TimedOut) {
            start_quarantine_required_ = true;
            return runtime_status(StatusCode::Timeout, "Plugin activation call timed out");
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
            Clock::now() + options_.stop_progress_timeout);
        if (outcome.status == BoundedCallOutcome::Outcome::TimedOut) {
            start_quarantine_required_ = true;
            return runtime_status(StatusCode::Timeout, "Plugin activation call timed out");
        }
        if (outcome.status == BoundedCallOutcome::Outcome::Failed) {
            return std::move(outcome.error);
        }
        if (!error->ok()) {
            return std::move(*error);
        }
        return std::move(value->value());
    }

    Result<void> start_activation() {
        auto manager = std::make_shared<plugin::PluginManager>();

        auto storage_library = manager->load_library(options_.storage.path);
        if (!storage_library.ok()) {
            return std::move(storage_library).status();
        }
        auto storage = supervise_start_value<plugin::StoragePluginInstance>(
            [manager, library = std::move(storage_library.value())]() mutable {
                return manager->create_storage_plugin_instance(std::move(library));
            });
        if (!storage.ok()) {
            return std::move(storage).status();
        }
        active_->storage_instance().emplace(std::move(storage.value()));
        storage_state_ = StorageActivationState::Created;

        auto storage_registry = std::make_shared<plugin::ServiceRegistry>(6, 128);
        auto registered = active_->register_storage_services(*storage_registry);
        if (!registered.ok()) {
            return registered;
        }
        auto frozen = storage_registry->freeze();
        if (!frozen.ok()) {
            return frozen;
        }
        std::shared_ptr<ActiveRuntimeState> state = active_;
        auto bound = supervise_start_call([state, storage_registry]() {
            return state->storage_instance()->bind(storage_registry->abi_view());
        });
        if (!bound.ok()) {
            return bound;
        }
        storage_state_ = StorageActivationState::Bound;
        auto control = supervise_start_value<rodaq_interface>(
            [state]() { return state->storage_instance()->required_control_interface(); });
        if (!control.ok()) {
            return std::move(control).status();
        }
        active_->set_storage_control(control.value());
        auto storage_config = options_.storage.config;
        auto configured = supervise_start_call([state, storage_config]() mutable {
            return state->storage_instance()->configure(storage_config.view().abi_view());
        });
        if (!configured.ok()) {
            return configured;
        }
        storage_state_ = StorageActivationState::Configured;
        auto capabilities = supervise_start_value<rodaq_storage_capabilities>(
            [state]() { return state->storage_capabilities(); });
        if (!capabilities.ok()) {
            return std::move(capabilities).status();
        }
        if (capabilities->flags != 0 || capabilities->reserved != 0 ||
            !rodaq::is_valid_abi_bool(capabilities->full_fidelity) ||
            capabilities->full_fidelity != RODAQ_TRUE ||
            capabilities->maximum_inflight_records == 0 ||
            capabilities->maximum_inflight_bytes == 0) {
            return runtime_status(StatusCode::InvalidArgument, "Storage capabilities are invalid");
        }
        std::size_t effective_payload_limit = options_.resources.max_single_payload_bytes;
        if (capabilities->maximum_payload_bytes != 0) {
            effective_payload_limit = std::min(
                effective_payload_limit, bounded_size(capabilities->maximum_payload_bytes));
        }
        effective_payload_limit =
            std::min(effective_payload_limit, bounded_size(capabilities->maximum_inflight_bytes));
        if (effective_payload_limit == 0) {
            return runtime_status(
                StatusCode::InvalidArgument, "Effective Storage payload limit is zero");
        }

        const std::size_t effective_capacity = std::min(
            {options_.resources.operation_capacity, options_.resources.completion_capacity,
             bounded_size(capabilities->maximum_inflight_records)});
        if (effective_capacity == 0) {
            return runtime_status(
                StatusCode::InvalidArgument, "Effective Storage capacity is zero");
        }
        const std::size_t effective_completion_capacity = effective_capacity;
        const std::size_t effective_payload_inflight_bytes = std::min(
            saturating_product(effective_capacity, effective_payload_limit),
            bounded_size(capabilities->maximum_inflight_bytes));

        effective_storage_handoff_timeout_ = effective_control_timeout(
            options_.stop_progress_timeout, capabilities->handoff_progress_timeout_ns);
        const auto effective_descriptor_timeout = effective_control_timeout(
            options_.stop_progress_timeout, capabilities->descriptor_progress_timeout_ns);
        effective_storage_flush_timeout_ = effective_control_timeout(
            options_.stop_progress_timeout, capabilities->flush_progress_timeout_ns);
        effective_storage_close_timeout_ = effective_control_timeout(
            options_.stop_progress_timeout, capabilities->close_progress_timeout_ns);

        active_->options().resources.max_single_payload_bytes = effective_payload_limit;
        active_->options().resources.dispatch_capacity = effective_capacity;
        active_->options().resources.operation_capacity = effective_capacity;
        active_->options().resources.completion_capacity = effective_completion_capacity;
        active_->set_descriptor_progress_timeout(effective_descriptor_timeout);
        active_->set_binding_progress_timeout(options_.stop_progress_timeout);

        auto opened = supervise_start_call([state]() {
            const RuntimeConfig& active_options = state->options();
            rodaq_storage_open_request open_request{};
            open_request.struct_size = sizeof(open_request);
            open_request.session_id = rodaq_string_view{
                active_options.session_id.data(), active_options.session_id.size()};
            open_request.target_uri = rodaq_string_view{
                active_options.target_uri.data(), active_options.target_uri.size()};
            return state->storage_instance()->open(open_request);
        });
        if (!opened.ok()) {
            return opened;
        }
        storage_open_ = true;
        storage_state_ = StorageActivationState::Opened;

        auto pipeline = active_->initialize_pipeline(
            effective_payload_limit, effective_payload_inflight_bytes,
            &active_->storage_instance().value(), effective_storage_handoff_timeout_);
        if (!pipeline.ok()) {
            return pipeline;
        }

        if (options_.acquisitions.size() != active_->options().acquisitions.size()) {
            return runtime_status(
                StatusCode::InternalError, "Acquisition count changed during activation");
        }
        for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
            auto library = manager->load_library(options_.acquisitions[index].path);
            if (!library.ok()) {
                return std::move(library).status();
            }
            auto instance = supervise_start_value<plugin::AcquisitionPluginInstance>(
                [manager, library = std::move(library.value())]() mutable {
                    return manager->create_acquisition_plugin_instance(std::move(library));
                });
            if (!instance.ok()) {
                return std::move(instance).status();
            }
            active_->acquisition_instance(index).emplace(std::move(instance.value()));
            acquisition_states_[index] = AcquisitionActivationState::Created;

            auto registry = std::make_shared<plugin::ServiceRegistry>(4, 128);
            auto acquisition_registered = active_->register_acquisition_services(*registry, index);
            if (!acquisition_registered.ok()) {
                return acquisition_registered;
            }
            auto acquisition_frozen = registry->freeze();
            if (!acquisition_frozen.ok()) {
                return acquisition_frozen;
            }
            auto acquisition_bound = supervise_start_call([state, registry, index]() {
                return state->acquisition_instance(index)->bind(registry->abi_view());
            });
            if (!acquisition_bound.ok()) {
                return acquisition_bound;
            }
            acquisition_states_[index] = AcquisitionActivationState::Bound;
            auto acquisition_control = supervise_start_value<rodaq_interface>([state, index]() {
                return state->acquisition_instance(index)->required_control_interface();
            });
            if (!acquisition_control.ok()) {
                return std::move(acquisition_control).status();
            }
            active_->set_acquisition_control(index, acquisition_control.value());
            auto acquisition_config = options_.acquisitions[index].config;
            auto acquisition_configured = supervise_start_call(
                [state, acquisition_config = std::move(acquisition_config), index]() mutable {
                    return state->acquisition_instance(index)->configure(
                        acquisition_config.view().abi_view());
                });
            if (!acquisition_configured.ok()) {
                return acquisition_configured;
            }
            acquisition_states_[index] = AcquisitionActivationState::Configured;
        }

        for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
            acquisition_states_[index] = AcquisitionActivationState::StartAttempted;
            auto started = supervise_start_call(
                [state, index]() { return state->acquisition_instance(index)->start(); });
            if (!started.ok()) {
                return started;
            }
            acquisition_states_[index] = AcquisitionActivationState::Started;
        }
        auto descriptor_wait = outside_lifecycle([&] {
            return active_->wait_for_descriptor_reconciliation(
                Clock::now() + options_.stop_progress_timeout + options_.stop_progress_timeout);
        });
        if (!descriptor_wait.ok()) {
            return descriptor_wait;
        }
        return {};
    }

    void abort_start() noexcept {
        aborting_ = true;
        if (active_ == nullptr) {
            return;
        }
        active_->mark_stopping();
        const bool descriptor_worker_closed = outside_lifecycle([&] {
            return active_->close_and_join_descriptor_reconciliation(
                Clock::now() + options_.stop_progress_timeout);
        });
        if (start_quarantine_required_ || active_->requires_quarantine() ||
            !descriptor_worker_closed) {
            capture_activation_outcome();
            quarantined_ = std::move(active_);
            start_quarantine_required_ = false;
            state_ = RuntimeState::Failed;
            return;
        }
        if (active_->pipeline() != nullptr) {
            active_->pipeline()->close_admission();
            active_->pipeline()->close_dispatch();
            active_->pipeline()->join_dispatch();
            active_->pipeline()->close_callbacks();
            active_->pipeline()->join_completion();
        }
        bool unsafe_to_release = false;
        std::shared_ptr<ActiveRuntimeState> state = active_;
        for (std::size_t offset = 0; offset < acquisition_states_.size(); ++offset) {
            const std::size_t index = acquisition_states_.size() - offset - 1U;
            const AcquisitionActivationState activation_state = acquisition_states_[index];
            if (!active_->acquisition_instance(index).has_value() ||
                (activation_state != AcquisitionActivationState::StartAttempted &&
                 activation_state != AcquisitionActivationState::Started &&
                 activation_state != AcquisitionActivationState::Quiesced)) {
                continue;
            }

            if (activation_state != AcquisitionActivationState::Quiesced) {
                auto quiesced = run_bounded_call(
                    "abort-quiesce",
                    [state, index]() -> Result<void> {
                        auto control =
                            state->acquisition_instance(index)->required_control_interface();
                        if (!control.ok()) {
                            return std::move(control).status();
                        }
                        const auto* table =
                            static_cast<const rodaq_acquisition_control_v1*>(control->vtable);
                        rodaq_status error{};
                        const rodaq_status_code status =
                            table->quiesce_plugin(control->context, &error);
                        if (status != RODAQ_STATUS_OK) {
                            return abi_failure_status(
                                status, error, "Acquisition quiescence failed during abort");
                        }
                        return {};
                    },
                    Clock::now() + options_.stop_progress_timeout);
                if (quiesced.status != BoundedCallOutcome::Outcome::Completed) {
                    active_->request_failure(
                        quiesced.status == BoundedCallOutcome::Outcome::TimedOut
                            ? runtime_status(
                                  StatusCode::Timeout,
                                  "Acquisition quiescence timed out during activation abort")
                            : std::move(quiesced.error));
                    unsafe_to_release = true;
                    break;
                }
                acquisition_states_[index] = AcquisitionActivationState::Quiesced;
            }

            auto stopped = run_bounded_call(
                "abort-stop",
                [state, index]() { return state->acquisition_instance(index)->stop(); },
                Clock::now() + options_.stop_progress_timeout);
            if (stopped.status != BoundedCallOutcome::Outcome::Completed) {
                active_->request_failure(
                    stopped.status == BoundedCallOutcome::Outcome::TimedOut
                        ? runtime_status(
                              StatusCode::Timeout,
                              "Acquisition stop timed out during activation abort")
                        : std::move(stopped.error));
                unsafe_to_release = true;
                break;
            }
            acquisition_states_[index] = AcquisitionActivationState::Stopped;
        }
        if (!unsafe_to_release && storage_open_ &&
            storage_state_ == StorageActivationState::Opened &&
            active_->storage_instance().has_value()) {
            rodaq_storage_close_request close_request{};
            close_request.struct_size = sizeof(close_request);
            close_request.disposition = RODAQ_STORAGE_CLOSE_ACTIVATION_ABORT;
            auto closed = run_bounded_call(
                "abort-close",
                [state, close_request]() {
                    return state->storage_instance()->close(close_request);
                },
                Clock::now() + options_.stop_progress_timeout);
            if (closed.status == BoundedCallOutcome::Outcome::Completed) {
                storage_state_ = StorageActivationState::Closed;
            } else {
                active_->request_failure(
                    closed.status == BoundedCallOutcome::Outcome::TimedOut
                        ? runtime_status(
                              StatusCode::Timeout,
                              "Storage close timed out during activation abort")
                        : std::move(closed.error));
                storage_state_ = StorageActivationState::Quarantined;
                unsafe_to_release = true;
            }
        }
        if (!unsafe_to_release) {
            for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
                if (!active_->acquisition_instance(index).has_value() ||
                    !active_->acquisition_instance(index)->valid()) {
                    continue;
                }
                auto destroyed = run_bounded_call(
                    "abort-destroy-acquisition",
                    [state, index]() {
                        state->acquisition_instance(index)->destroy();
                        return Result<void>();
                    },
                    Clock::now() + options_.stop_progress_timeout);
                if (destroyed.status != BoundedCallOutcome::Outcome::Completed) {
                    active_->request_failure(
                        destroyed.status == BoundedCallOutcome::Outcome::TimedOut
                            ? runtime_status(
                                  StatusCode::Timeout,
                                  "Acquisition destroy timed out during activation abort")
                            : std::move(destroyed.error));
                    unsafe_to_release = true;
                    break;
                }
            }
        }
        if (!unsafe_to_release && active_->storage_instance().has_value() &&
            active_->storage_instance()->valid()) {
            auto destroyed = run_bounded_call(
                "abort-destroy-storage",
                [state]() {
                    state->storage_instance()->destroy();
                    return Result<void>();
                },
                Clock::now() + options_.stop_progress_timeout);
            if (destroyed.status != BoundedCallOutcome::Outcome::Completed) {
                active_->request_failure(
                    destroyed.status == BoundedCallOutcome::Outcome::TimedOut
                        ? runtime_status(
                              StatusCode::Timeout,
                              "Storage destroy timed out during activation abort")
                        : std::move(destroyed.error));
                unsafe_to_release = true;
            }
        }
        if (!outside_lifecycle([&] {
                return active_->shutdown_logging(Clock::now() + options_.logging.shutdown_timeout);
            })) {
            active_->quarantine_logging("Runtime logging shutdown timed out during abort");
            active_->record_secondary_failure(runtime_status(
                StatusCode::Timeout, "Runtime logging shutdown timed out during activation abort"));
        }
        if (unsafe_to_release) {
            capture_activation_outcome();
            quarantined_ = std::move(active_);
        } else {
            capture_activation_outcome();
            active_.reset();
            release_activation_slot();
        }
        if (!unsafe_to_release) {
            std::fill(
                acquisition_states_.begin(), acquisition_states_.end(),
                AcquisitionActivationState::Empty);
            storage_state_ = StorageActivationState::Empty;
        }
        if (!unsafe_to_release) {
            storage_open_ = false;
        }
        start_quarantine_required_ = false;
        state_ = RuntimeState::Failed;
    }

    bool recover_activation() {
        LifecycleOwner owner(*this);
        active_->reap_quarantined_workers();
        if (recovery_abandoned_) {
            return false;
        }
        if (pending_call_ != nullptr && !pending_call_->joined->load(std::memory_order_acquire)) {
            return false;
        }
        if (active_->requires_quarantine()) {
            return false;
        }
        if (pending_call_ != nullptr && pending_call_->site.find("destroy") != std::string::npos &&
            pending_call_->result.ok()) {
            pending_call_.reset();
        }
        if (aborting_) {
            // A timed-out activation call has finished; its original output was
            // never published, so abort now owns only the published instances.
            if (pending_call_ != nullptr && (pending_call_->site == "activation-call" ||
                                             pending_call_->site == "activation-value")) {
                pending_call_.reset();
            }
            start_quarantine_required_ = false;
            abort_start();
        } else {
            static_cast<void>(stop_internal());
        }
        if (quarantined_ != nullptr) {
            active_ = std::move(quarantined_);
        }
        return active_ == nullptr;
    }

    void transfer_quarantine() {
        auto recovery = std::make_shared<Impl>(options_);
        recovery->active_ = quarantined_;
        recovery->state_ = RuntimeState::Failed;
        recovery->recovering_ = true;
        recovery->activation_generation_ = activation_generation_;
        recovery->activation_slot_ = activation_slot_;
        recovery->acquisition_states_ = acquisition_states_;
        recovery->storage_state_ = storage_state_;
        recovery->storage_open_ = storage_open_;
        recovery->storage_flushed_ = storage_flushed_;
        recovery->aborting_ = aborting_;
        recovery->recovery_abandoned_ = recovery_abandoned_;
        recovery->pending_call_ = std::move(pending_call_);
        recovery->effective_storage_flush_timeout_ = effective_storage_flush_timeout_;
        recovery->effective_storage_close_timeout_ = effective_storage_close_timeout_;
        std::weak_ptr<Impl> weak = recovery;
        if (!QuarantineManager::process().retain_activation(
                *activation_slot_, recovery, "Runtime activation quarantine", [weak] {
                    auto owner = weak.lock();
                    return owner != nullptr && owner->recover_activation();
                })) {
            std::abort();
        }
        activation_slot_.reset();
    }

    Result<void> quarantine_failure() {
        if (active_ != nullptr) {
            quarantined_ = std::move(active_);
        }
        state_ = RuntimeState::Failed;
        if (activation_slot_.has_value() && !recovering_) {
            transfer_quarantine();
        }
        return runtime_status(StatusCode::Unavailable, "Runtime stop requires quarantine");
    }

    void capture_activation_outcome() noexcept {
        if (active_ == nullptr) {
            return;
        }
        try {
            active_->record_stop_latency();
            last_stats_ = active_->stats();
            auto payload_stats = active_->payload_pool_statistics();
            if (payload_stats.ok()) {
                last_payload_stats_ = std::move(payload_stats.value());
            }
            auto primary = active_->primary_failure();
            if (!primary.ok()) {
                auto secondary = active_->secondary_failures();
                if (secondary.ok()) {
                    last_secondary_failures_ = std::move(secondary.value());
                }
                last_secondary_failure_overflow_count_ =
                    active_->secondary_failure_overflow_count();
                last_primary_failure_ = std::move(primary);
            }
        } catch (...) {
        }
    }

    void release_activation_slot() noexcept {
        if (activation_slot_.has_value()) {
            QuarantineManager::process().release_activation(*activation_slot_);
            activation_slot_.reset();
        }
    }

    RuntimeConfig options_;
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_condition_;
    bool lifecycle_owned_ = false;
    std::shared_ptr<FailureSignal> failure_signal_ = std::make_shared<FailureSignal>();
    std::thread failure_worker_;
    RuntimeState state_ = RuntimeState::Constructed;
    std::uint64_t activation_generation_ = 0;
    std::shared_ptr<ActiveRuntimeState> active_;
    std::shared_ptr<ActiveRuntimeState> quarantined_;
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
    std::optional<RuntimeStatsSnapshot> last_stats_;
    std::optional<PayloadPoolStats> last_payload_stats_;
    Status last_primary_failure_;
    std::vector<Status> last_secondary_failures_;
    std::uint64_t last_secondary_failure_overflow_count_ = 0;
    std::chrono::nanoseconds effective_storage_handoff_timeout_{};
    std::chrono::nanoseconds effective_storage_flush_timeout_{};
    std::chrono::nanoseconds effective_storage_close_timeout_{};
};

Runtime::Runtime(RuntimeConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Runtime::~Runtime() = default;

Result<void> Runtime::configure() { return impl_->configure(); }

Result<void> Runtime::start() { return impl_->start(); }

Result<void> Runtime::stop() { return impl_->stop(); }

RuntimeState Runtime::state() const noexcept { return impl_->state(); }

Result<rodaq_runtime_lifecycle_snapshot> Runtime::lifecycle_snapshot() const {
    return impl_->lifecycle_snapshot();
}

Status Runtime::primary_failure() const { return impl_->primary_failure(); }

Result<std::vector<Status>> Runtime::secondary_failures() const {
    return impl_->secondary_failures();
}

std::uint64_t Runtime::secondary_failure_overflow_count() const noexcept {
    return impl_->secondary_failure_overflow_count();
}

Result<RuntimeStatsSnapshot> Runtime::stats() const { return impl_->stats(); }

Result<PayloadPoolStats> Runtime::payload_stats() const { return impl_->payload_stats(); }

}  // namespace rodaq::runtime
