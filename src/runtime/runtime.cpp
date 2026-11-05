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

Error runtime_error(ErrorCode code, std::string message) {
    return Error(code, "runtime", 0, std::move(message));
}

template <typename Config>
Result<void> validate_plugin_options(const Config& options) {
    std::error_code error;
    if (options.path.empty() || !std::filesystem::is_regular_file(options.path, error)) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::NotFound, "Runtime Plugin path is missing"));
    }
    const ConfigNodeView view = options.config.view();
    if (view.AbiView().context == nullptr) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Runtime Plugin config is empty"));
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
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Runtime capacity configuration is invalid"));
    }
    if (resources.dispatch_capacity != resources.operation_capacity) {
        return nonstd::make_unexpected(runtime_error(
            ErrorCode::InvalidArgument, "V1 requires equal dispatch and operation capacity"));
    }
    if (pool_config.classes.empty()) {
        return nonstd::make_unexpected(runtime_error(
            ErrorCode::InvalidArgument, "PayloadPool configuration has no size classes"));
    }
    std::size_t previous_class_size = 0;
    for (const PayloadClassConfig& payload_class : pool_config.classes) {
        if (payload_class.max_payload_bytes == 0 || payload_class.block_count == 0 ||
            payload_class.max_payload_bytes <= previous_class_size ||
            payload_class.max_payload_bytes >
                std::numeric_limits<std::size_t>::max() / payload_class.block_count) {
            return nonstd::make_unexpected(runtime_error(
                ErrorCode::InvalidArgument, "PayloadPool size class configuration is invalid"));
        }
        previous_class_size = payload_class.max_payload_bytes;
    }
    if (pool_config.classes.back().max_payload_bytes < resources.max_single_payload_bytes) {
        return nonstd::make_unexpected(runtime_error(
            ErrorCode::InvalidArgument,
            "PayloadPool capacity is below the configured payload limit"));
    }
    if (options.session_id.empty() || options.target_uri.empty()) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Storage open configuration is empty"));
    }
    if (options.acquisitions.empty()) {
        return nonstd::make_unexpected(runtime_error(
            ErrorCode::InvalidArgument, "At least one Acquisition Plugin is required"));
    }
    auto storage = validate_plugin_options(options.storage);
    if (!storage.has_value()) {
        return storage;
    }
    for (const PluginConfig& acquisition : options.acquisitions) {
        auto validated = validate_plugin_options(acquisition);
        if (!validated.has_value()) {
            return validated;
        }
    }
    return {};
}

Error abi_error(rodaq_status status, const rodaq_error& error, std::string_view fallback) {
    auto converted = result_from_status(status, &error, "runtime");
    if (!converted.has_value()) {
        return std::move(converted.error());
    }
    return runtime_error(ErrorCode::InternalError, std::string(fallback));
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
    enum class Status {
        Completed,
        Failed,
        TimedOut,
    };

    Status status = Status::Failed;
    std::optional<Error> error;
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
    Callable callable, std::chrono::steady_clock::time_point deadline) noexcept {
    auto managed = QuarantineManager::Process().Run(
        std::function<Result<void>()>(std::move(callable)), deadline,
        "Runtime Plugin control call");
    if (!managed.has_value()) {
        return BoundedCallOutcome{BoundedCallOutcome::Status::Failed, managed.error()};
    }
    switch (managed.value().status) {
        case SupervisedCallStatus::Completed:
            return BoundedCallOutcome{BoundedCallOutcome::Status::Completed, std::nullopt};
        case SupervisedCallStatus::Failed:
            return BoundedCallOutcome{
                BoundedCallOutcome::Status::Failed, std::move(managed.value().error.value())};
        case SupervisedCallStatus::TimedOut:
            return BoundedCallOutcome{BoundedCallOutcome::Status::TimedOut, std::nullopt};
    }
    return BoundedCallOutcome{
        BoundedCallOutcome::Status::Failed,
        runtime_error(ErrorCode::InternalError, "Unknown supervised call outcome")};
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

    Result<void> Configure() {
        LifecycleOwner owner(*this);
        if (state_ != RuntimeState::Constructed) {
            return nonstd::make_unexpected(runtime_error(
                ErrorCode::InvalidState, "Runtime cannot be configured from its state"));
        }
        auto validated = validate_options(options_);
        if (!validated.has_value()) {
            return validated;
        }
        state_ = RuntimeState::Configured;
        return {};
    }

    Result<void> Start() {
        LifecycleOwner owner(*this);
        if (state_ != RuntimeState::Configured && state_ != RuntimeState::Stopped) {
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::InvalidState, "Runtime cannot start from its state"));
        }
        if (quarantined_ != nullptr) {
            return nonstd::make_unexpected(runtime_error(
                ErrorCode::Unavailable,
                "Runtime restart is blocked by quarantined callback state"));
        }
        if (activation_generation_ == std::numeric_limits<std::uint32_t>::max()) {
            return nonstd::make_unexpected(runtime_error(
                ErrorCode::ResourceExhausted, "Runtime activation generation is exhausted"));
        }
        auto activation_slot = QuarantineManager::Process().ReserveActivation("Runtime activation");
        if (!activation_slot.has_value()) {
            return nonstd::make_unexpected(std::move(activation_slot.error()));
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
        auto created = ActiveRuntimeState::Create(options_, activation_generation_);
        if (!created.has_value()) {
            release_activation_slot();
            state_ = RuntimeState::Failed;
            return nonstd::make_unexpected(std::move(created.error()));
        }
        active_ = std::move(created.value());
        active_->SetFailureSignal(failure_signal_);
        if (!failure_worker_.joinable()) {
            try {
                failure_worker_ = std::thread([this] { failure_loop(); });
            } catch (const std::exception& error) {
                active_->RequestFailure(runtime_error(ErrorCode::Unavailable, error.what()));
                abort_start();
                return nonstd::make_unexpected(runtime_error(ErrorCode::Unavailable, error.what()));
            }
        }

        auto descriptor_worker = active_->StartDescriptorReconciliation();
        if (!descriptor_worker.has_value()) {
            active_->RequestFailure(descriptor_worker.error());
            abort_start();
            state_ = RuntimeState::Failed;
            return descriptor_worker;
        }
        auto started = start_activation();
        if (!started.has_value()) {
            if (!active_->HasPrimaryFailure()) {
                active_->RequestFailure(started.error());
            }
            abort_start();
            state_ = RuntimeState::Failed;
            return started;
        }
        if (active_->HasPrimaryFailure()) {
            auto error = active_->PrimaryFailure();
            abort_start();
            return nonstd::make_unexpected(std::move(error.value()));
        }
        if (!active_->MarkRunning()) {
            auto error = active_->PrimaryFailure();
            abort_start();
            return nonstd::make_unexpected(std::move(error.value()));
        }
        state_ = RuntimeState::Running;
        return {};
    }

    Result<void> Stop() {
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
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::InvalidState, "Runtime cannot stop from its state"));
        }
        if (active_ == nullptr) {
            if (quarantined_ != nullptr || state_ == RuntimeState::Failed) {
                return nonstd::make_unexpected(
                    runtime_error(ErrorCode::Unavailable, "Runtime stop requires quarantine"));
            }
            state_ = RuntimeState::Stopped;
            return {};
        }

        state_ = RuntimeState::Stopping;
        active_->MarkStopping();
        if (active_->HasPrimaryFailure() && active_->pipeline() != nullptr) {
            active_->pipeline()->BeginFailureCutoff();
        }
        if (!outside_lifecycle([&] {
                return active_->CloseAndJoinDescriptorReconciliation(
                    Clock::now() + options_.stop_progress_timeout);
            })) {
            return quarantine_failure();
        }
        if (active_->RequiresQuarantine()) {
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
                    if (!control.has_value()) {
                        return nonstd::make_unexpected(std::move(control.error()));
                    }
                    const auto* table =
                        static_cast<const rodaq_acquisition_control_v1*>(control->vtable);
                    rodaq_error error{};
                    const rodaq_status status = table->quiesce_plugin(control->context, &error);
                    if (status != RODAQ_STATUS_OK) {
                        return nonstd::make_unexpected(
                            abi_error(status, error, "Acquisition quiescence failed"));
                    }
                    return {};
                },
                Clock::now() + options_.stop_progress_timeout);
            if (outcome.status == BoundedCallOutcome::Status::TimedOut) {
                active_->RequestFailure(
                    runtime_error(ErrorCode::Timeout, "Acquisition quiescence timed out"));
                return quarantine_failure();
            }
            if (outcome.status == BoundedCallOutcome::Status::Failed) {
                active_->RequestFailure(std::move(outcome.error.value()));
                return quarantine_failure();
            }
            acquisition_states_[index] = AcquisitionActivationState::Quiesced;
        }

        if (!outside_lifecycle([&] {
                return active_->WaitForSubmissions(Clock::now() + options_.stop_progress_timeout);
            })) {
            active_->RequestFailure(
                runtime_error(ErrorCode::Timeout, "Runtime submission barrier timed out"));
            return quarantine_failure();
        }
        if (!outside_lifecycle([&] {
                return active_->WaitForAdmissionDrain(
                    Clock::now() + options_.stop_progress_timeout);
            })) {
            active_->RequestFailure(
                runtime_error(ErrorCode::Timeout, "Runtime admission barrier timed out"));
            return quarantine_failure();
        }

        StoragePipeline* pipeline = active_->pipeline();
        if (pipeline != nullptr) {
            pipeline->CloseDispatch();
            if (!outside_lifecycle([&] {
                    return pipeline->WaitDispatchIdle(
                        Clock::now() + options_.stop_progress_timeout);
                })) {
                active_->RequestFailure(
                    runtime_error(ErrorCode::Timeout, "Runtime dispatch barrier timed out"));
                return quarantine_failure();
            }
            outside_lifecycle([&] { return pipeline->JoinDispatch(); });
        }

        if (storage_open_ && !storage_flushed_ && active_->storage_instance().has_value()) {
            std::shared_ptr<ActiveRuntimeState> state = active_;
            auto flush = run_bounded_call(
                "flush", [state]() { return state->storage_instance()->flush(); },
                Clock::now() + effective_storage_flush_timeout_);
            if (flush.status == BoundedCallOutcome::Status::TimedOut) {
                active_->RequestFailure(
                    runtime_error(ErrorCode::Timeout, "Storage flush timed out"));
                return quarantine_failure();
            }
            storage_flushed_ = true;
            if (flush.status == BoundedCallOutcome::Status::Failed) {
                active_->RequestFailure(std::move(flush.error.value()));
            }
        }
        if (pipeline != nullptr && !outside_lifecycle([&] {
                return pipeline->WaitOperationsIdle(Clock::now() + options_.stop_progress_timeout);
            })) {
            active_->RequestFailure(
                runtime_error(ErrorCode::Timeout, "Runtime Storage operation barrier timed out"));
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
            if (close.status == BoundedCallOutcome::Status::TimedOut) {
                active_->RequestFailure(
                    runtime_error(ErrorCode::Timeout, "Storage close timed out"));
                return quarantine_failure();
            }
            storage_open_ = false;
            storage_state_ = StorageActivationState::Closed;
            if (close.status == BoundedCallOutcome::Status::Failed) {
                active_->RequestFailure(std::move(close.error.value()));
                storage_state_ = StorageActivationState::Quarantined;
                return quarantine_failure();
            }
        }
        if (pipeline != nullptr) {
            pipeline->ResolveAfterClose();
            pipeline->CloseCallbacks();
            outside_lifecycle([&] { return pipeline->JoinCompletion(); });
            pipeline->CloseRelease();
            const auto release_deadline = Clock::now() + options_.stop_progress_timeout;
            if (!outside_lifecycle([&] { return pipeline->WaitReleaseIdle(release_deadline); }) ||
                !outside_lifecycle(
                    [&] { return pipeline->WaitOperationsIdle(release_deadline); })) {
                if (!pipeline->RetainBlockedReleaseWorkers()) {
                    active_->RecordSecondaryFailure(runtime_error(
                        ErrorCode::ResourceExhausted,
                        "Runtime release worker quarantine capacity is exhausted"));
                }
                active_->RequestFailure(runtime_error(
                    ErrorCode::Timeout, "Runtime callback/release quiescence barrier timed out"));
                return quarantine_failure();
            }
            outside_lifecycle([&] { return pipeline->JoinRelease(); });
        }

        const bool unresolved = pipeline != nullptr && pipeline->HasUnresolvedOperations();
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
            if (stopped.status == BoundedCallOutcome::Status::TimedOut) {
                active_->RequestFailure(
                    runtime_error(ErrorCode::Timeout, "Acquisition stop timed out"));
                return quarantine_failure();
            }
            if (stopped.status == BoundedCallOutcome::Status::Failed) {
                active_->RequestFailure(std::move(stopped.error.value()));
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
            if (destroyed.status == BoundedCallOutcome::Status::TimedOut) {
                active_->RequestFailure(
                    runtime_error(ErrorCode::Timeout, "Acquisition destroy timed out"));
                return quarantine_failure();
            }
            if (destroyed.status == BoundedCallOutcome::Status::Failed) {
                active_->RequestFailure(std::move(destroyed.error.value()));
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
            if (destroyed.status == BoundedCallOutcome::Status::TimedOut) {
                active_->RequestFailure(
                    runtime_error(ErrorCode::Timeout, "Storage destroy timed out"));
                return quarantine_failure();
            }
            if (destroyed.status == BoundedCallOutcome::Status::Failed) {
                active_->RequestFailure(std::move(destroyed.error.value()));
                return quarantine_failure();
            }
        }
        if (!outside_lifecycle([&] {
                return active_->ShutdownLogging(Clock::now() + options_.logging.shutdown_timeout);
            })) {
            active_->QuarantineLogging("Runtime logging shutdown timed out");
            active_->RecordSecondaryFailure(
                runtime_error(ErrorCode::Timeout, "Runtime logging shutdown timed out"));
        }

        const bool failed =
            active_->state() == RuntimeState::Failed || active_->PrimaryFailure().has_value();
        active_->RecordStopLatency();
        last_stats_ = active_->Stats();
        auto payload_stats = active_->PayloadPoolStatistics();
        if (payload_stats.has_value()) {
            last_payload_stats_ = std::move(payload_stats.value());
        }
        auto primary_failure = active_->PrimaryFailure();
        if (primary_failure.has_value()) {
            auto secondary_failures = active_->SecondaryFailures();
            if (secondary_failures.has_value()) {
                last_secondary_failures_ = std::move(secondary_failures.value());
            }
            last_secondary_failure_overflow_count_ = active_->SecondaryFailureOverflowCount();
            last_primary_failure_ = std::move(primary_failure.value());
        }

        for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
            active_->acquisition_instance(index).reset();
        }
        active_->storage_instance().reset();
        active_->MarkStopped();
        active_.reset();
        release_activation_slot();
        state_ = failed ? RuntimeState::Failed : RuntimeState::Stopped;
        if (failed) {
            state_ = RuntimeState::Failed;
        }
        return failed ? Result<void>(nonstd::make_unexpected(
                            runtime_error(ErrorCode::Unavailable, "Runtime stopped after failure")))
                      : Result<void>();
    }

public:
    RuntimeState State() const noexcept {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ != nullptr && active_->state() == RuntimeState::Failed) {
            return RuntimeState::Failed;
        }
        return state_;
    }

    Result<rodaq_runtime_lifecycle_snapshot> LifecycleSnapshot() const {
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
        return active_->LifecycleSnapshot();
    }

    Result<Error> PrimaryFailure() const {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ == nullptr) {
            if (quarantined_ != nullptr) {
                return quarantined_->PrimaryFailure();
            }
            if (last_primary_failure_.has_value()) {
                return last_primary_failure_.value();
            }
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::NotFound, "Runtime has no primary failure"));
        }
        return active_->PrimaryFailure();
    }

    Result<std::vector<Error>> SecondaryFailures() const {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ != nullptr) {
            return active_->SecondaryFailures();
        }
        if (quarantined_ != nullptr) {
            return quarantined_->SecondaryFailures();
        }
        if (last_primary_failure_.has_value()) {
            return last_secondary_failures_;
        }
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidState, "Runtime has no failure history"));
    }

    std::uint64_t SecondaryFailureOverflowCount() const noexcept {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ != nullptr) {
            return active_->SecondaryFailureOverflowCount();
        }
        if (quarantined_ != nullptr) {
            return quarantined_->SecondaryFailureOverflowCount();
        }
        return last_secondary_failure_overflow_count_;
    }

    Result<RuntimeStatsSnapshot> Stats() const {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ != nullptr) {
            return active_->Stats();
        }
        if (quarantined_ != nullptr) {
            return quarantined_->Stats();
        }
        if (last_stats_.has_value()) {
            return last_stats_.value();
        }
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidState, "Runtime has no active statistics"));
    }

    Result<PayloadPoolStats> PayloadStats() const {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ != nullptr) {
            return active_->PayloadPoolStatistics();
        }
        if (quarantined_ != nullptr) {
            return quarantined_->PayloadPoolStatistics();
        }
        if (last_payload_stats_.has_value()) {
            return last_payload_stats_.value();
        }
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidState, "Runtime has no active Payload statistics"));
    }

    ~Impl() {
        // Close the lifetime-safe mailbox before joining the sole supervisor.
        failure_signal_->Close();
        if (failure_worker_.joinable()) {
            failure_worker_.join();
        }
        QuarantineManager::Process().ReapCompleted();
        if (active_ != nullptr) {
            static_cast<void>(Stop());
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
        QuarantineManager::Process().ReapCompleted();
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
        std::optional<Result<void>> result;
        std::string site;
    };

    template <typename Callable>
    BoundedCallOutcome run_bounded_call(
        const char* site, Callable callable, Clock::time_point deadline) noexcept {
        if (pending_call_ != nullptr) {
            if (pending_call_->site != site ||
                !pending_call_->done.load(std::memory_order_acquire)) {
                return {BoundedCallOutcome::Status::TimedOut, std::nullopt};
            }
            auto pending = std::move(pending_call_);
            if (!pending->result->has_value()) {
                recovery_abandoned_ = true;
                return {BoundedCallOutcome::Status::Failed, pending->result->error()};
            }
            return {BoundedCallOutcome::Status::Completed, std::nullopt};
        }
        auto pending = std::make_shared<PendingCall>();
        pending->site = site;
        auto outcome = outside_lifecycle([&] {
            return supervise_call(
                [pending, callable = std::move(callable)]() mutable {
                    try {
                        pending->result.emplace(callable());
                    } catch (...) {
                        pending->result.emplace(nonstd::make_unexpected(
                            runtime_error(ErrorCode::InternalError, "Plugin control call threw")));
                    }
                    pending->done.store(true, std::memory_order_release);
                    return *pending->result;
                },
                deadline);
        });
        if (outcome.status == BoundedCallOutcome::Status::TimedOut) {
            pending_call_ = std::move(pending);
        } else if (outcome.status == BoundedCallOutcome::Status::Failed) {
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
            if (active_ != nullptr && active_->HasPrimaryFailure()) {
                static_cast<void>(stop_internal());
            }
        }
    }

    template <typename Callable>
    Result<void> supervise_start_call(Callable callable) {
        auto outcome = run_bounded_call(
            "activation-call", std::move(callable), Clock::now() + options_.stop_progress_timeout);
        if (outcome.status == BoundedCallOutcome::Status::TimedOut) {
            start_quarantine_required_ = true;
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::Timeout, "Plugin activation call timed out"));
        }
        if (outcome.status == BoundedCallOutcome::Status::Failed) {
            return nonstd::make_unexpected(std::move(outcome.error.value()));
        }
        return {};
    }

    template <typename T, typename Callable>
    Result<T> supervise_start_value(Callable callable) {
        auto value = std::make_shared<std::optional<T>>();
        auto error = std::make_shared<std::optional<Error>>();
        auto outcome = run_bounded_call(
            "activation-value",
            [callable = std::move(callable), value, error]() mutable -> Result<void> {
                auto result = callable();
                if (result.has_value()) {
                    value->emplace(std::move(result.value()));
                } else {
                    error->emplace(std::move(result.error()));
                }
                return {};
            },
            Clock::now() + options_.stop_progress_timeout);
        if (outcome.status == BoundedCallOutcome::Status::TimedOut) {
            start_quarantine_required_ = true;
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::Timeout, "Plugin activation call timed out"));
        }
        if (outcome.status == BoundedCallOutcome::Status::Failed) {
            return nonstd::make_unexpected(std::move(outcome.error.value()));
        }
        if (error->has_value()) {
            return nonstd::make_unexpected(std::move(error->value()));
        }
        return std::move(value->value());
    }

    Result<void> start_activation() {
        auto manager = std::make_shared<plugin::PluginManager>();

        auto storage_library = manager->load_library(options_.storage.path);
        if (!storage_library.has_value()) {
            return nonstd::make_unexpected(std::move(storage_library.error()));
        }
        auto storage = supervise_start_value<plugin::StoragePluginInstance>(
            [manager, library = std::move(storage_library.value())]() mutable {
                return manager->create_storage_plugin_instance(std::move(library));
            });
        if (!storage.has_value()) {
            return nonstd::make_unexpected(std::move(storage.error()));
        }
        active_->storage_instance().emplace(std::move(storage.value()));
        storage_state_ = StorageActivationState::Created;

        auto storage_registry = std::make_shared<plugin::ServiceRegistry>(6, 128);
        auto registered = active_->RegisterStorageServices(*storage_registry);
        if (!registered.has_value()) {
            return registered;
        }
        auto frozen = storage_registry->Freeze();
        if (!frozen.has_value()) {
            return frozen;
        }
        std::shared_ptr<ActiveRuntimeState> state = active_;
        auto bound = supervise_start_call([state, storage_registry]() {
            return state->storage_instance()->bind(storage_registry->AbiView());
        });
        if (!bound.has_value()) {
            return bound;
        }
        storage_state_ = StorageActivationState::Bound;
        auto control = supervise_start_value<rodaq_interface>(
            [state]() { return state->storage_instance()->required_control_interface(); });
        if (!control.has_value()) {
            return nonstd::make_unexpected(std::move(control.error()));
        }
        active_->SetStorageControl(control.value());
        auto storage_config = options_.storage.config;
        auto configured = supervise_start_call([state, storage_config]() mutable {
            return state->storage_instance()->configure(storage_config.view().AbiView());
        });
        if (!configured.has_value()) {
            return configured;
        }
        storage_state_ = StorageActivationState::Configured;
        auto capabilities = supervise_start_value<rodaq_storage_capabilities>(
            [state]() { return state->StorageCapabilities(); });
        if (!capabilities.has_value()) {
            return nonstd::make_unexpected(std::move(capabilities.error()));
        }
        if (capabilities->flags != 0 || capabilities->reserved != 0 ||
            !rodaq::is_valid_abi_bool(capabilities->full_fidelity) ||
            capabilities->full_fidelity != RODAQ_TRUE ||
            capabilities->maximum_inflight_records == 0 ||
            capabilities->maximum_inflight_bytes == 0) {
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::InvalidArgument, "Storage capabilities are invalid"));
        }
        std::size_t effective_payload_limit = options_.resources.max_single_payload_bytes;
        if (capabilities->maximum_payload_bytes != 0) {
            effective_payload_limit = std::min(
                effective_payload_limit, bounded_size(capabilities->maximum_payload_bytes));
        }
        effective_payload_limit =
            std::min(effective_payload_limit, bounded_size(capabilities->maximum_inflight_bytes));
        if (effective_payload_limit == 0) {
            return nonstd::make_unexpected(runtime_error(
                ErrorCode::InvalidArgument, "Effective Storage payload limit is zero"));
        }

        const std::size_t effective_capacity = std::min(
            {options_.resources.operation_capacity, options_.resources.completion_capacity,
             bounded_size(capabilities->maximum_inflight_records)});
        if (effective_capacity == 0) {
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::InvalidArgument, "Effective Storage capacity is zero"));
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
        active_->SetDescriptorProgressTimeout(effective_descriptor_timeout);
        active_->SetBindingProgressTimeout(options_.stop_progress_timeout);

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
        if (!opened.has_value()) {
            return opened;
        }
        storage_open_ = true;
        storage_state_ = StorageActivationState::Opened;

        auto pipeline = active_->InitializePipeline(
            effective_payload_limit, effective_payload_inflight_bytes,
            &active_->storage_instance().value(), effective_storage_handoff_timeout_);
        if (!pipeline.has_value()) {
            return pipeline;
        }

        if (options_.acquisitions.size() != active_->options().acquisitions.size()) {
            return nonstd::make_unexpected(runtime_error(
                ErrorCode::InternalError, "Acquisition count changed during activation"));
        }
        for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
            auto library = manager->load_library(options_.acquisitions[index].path);
            if (!library.has_value()) {
                return nonstd::make_unexpected(std::move(library.error()));
            }
            auto instance = supervise_start_value<plugin::AcquisitionPluginInstance>(
                [manager, library = std::move(library.value())]() mutable {
                    return manager->create_acquisition_plugin_instance(std::move(library));
                });
            if (!instance.has_value()) {
                return nonstd::make_unexpected(std::move(instance.error()));
            }
            active_->acquisition_instance(index).emplace(std::move(instance.value()));
            acquisition_states_[index] = AcquisitionActivationState::Created;

            auto registry = std::make_shared<plugin::ServiceRegistry>(4, 128);
            auto acquisition_registered = active_->RegisterAcquisitionServices(*registry, index);
            if (!acquisition_registered.has_value()) {
                return acquisition_registered;
            }
            auto acquisition_frozen = registry->Freeze();
            if (!acquisition_frozen.has_value()) {
                return acquisition_frozen;
            }
            auto acquisition_bound = supervise_start_call([state, registry, index]() {
                return state->acquisition_instance(index)->bind(registry->AbiView());
            });
            if (!acquisition_bound.has_value()) {
                return acquisition_bound;
            }
            acquisition_states_[index] = AcquisitionActivationState::Bound;
            auto acquisition_control = supervise_start_value<rodaq_interface>([state, index]() {
                return state->acquisition_instance(index)->required_control_interface();
            });
            if (!acquisition_control.has_value()) {
                return nonstd::make_unexpected(std::move(acquisition_control.error()));
            }
            active_->SetAcquisitionControl(index, acquisition_control.value());
            auto acquisition_config = options_.acquisitions[index].config;
            auto acquisition_configured = supervise_start_call(
                [state, acquisition_config = std::move(acquisition_config), index]() mutable {
                    return state->acquisition_instance(index)->configure(
                        acquisition_config.view().AbiView());
                });
            if (!acquisition_configured.has_value()) {
                return acquisition_configured;
            }
            acquisition_states_[index] = AcquisitionActivationState::Configured;
        }

        for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
            acquisition_states_[index] = AcquisitionActivationState::StartAttempted;
            auto started = supervise_start_call(
                [state, index]() { return state->acquisition_instance(index)->start(); });
            if (!started.has_value()) {
                return started;
            }
            acquisition_states_[index] = AcquisitionActivationState::Started;
        }
        auto descriptor_wait = outside_lifecycle([&] {
            return active_->WaitForDescriptorReconciliation(
                Clock::now() + options_.stop_progress_timeout + options_.stop_progress_timeout);
        });
        if (!descriptor_wait.has_value()) {
            return descriptor_wait;
        }
        return {};
    }

    void abort_start() noexcept {
        aborting_ = true;
        if (active_ == nullptr) {
            return;
        }
        active_->MarkStopping();
        const bool descriptor_worker_closed = outside_lifecycle([&] {
            return active_->CloseAndJoinDescriptorReconciliation(
                Clock::now() + options_.stop_progress_timeout);
        });
        if (start_quarantine_required_ || active_->RequiresQuarantine() ||
            !descriptor_worker_closed) {
            capture_activation_outcome();
            quarantined_ = std::move(active_);
            start_quarantine_required_ = false;
            state_ = RuntimeState::Failed;
            return;
        }
        if (active_->pipeline() != nullptr) {
            active_->pipeline()->CloseAdmission();
            active_->pipeline()->CloseDispatch();
            active_->pipeline()->JoinDispatch();
            active_->pipeline()->CloseCallbacks();
            active_->pipeline()->JoinCompletion();
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
                        if (!control.has_value()) {
                            return nonstd::make_unexpected(std::move(control.error()));
                        }
                        const auto* table =
                            static_cast<const rodaq_acquisition_control_v1*>(control->vtable);
                        rodaq_error error{};
                        const rodaq_status status = table->quiesce_plugin(control->context, &error);
                        return status == RODAQ_STATUS_OK
                                   ? Result<void>()
                                   : nonstd::make_unexpected(abi_error(
                                         status, error,
                                         "Acquisition quiescence failed during abort"));
                    },
                    Clock::now() + options_.stop_progress_timeout);
                if (quiesced.status != BoundedCallOutcome::Status::Completed) {
                    active_->RequestFailure(
                        quiesced.status == BoundedCallOutcome::Status::TimedOut
                            ? runtime_error(
                                  ErrorCode::Timeout,
                                  "Acquisition quiescence timed out during activation abort")
                            : std::move(quiesced.error.value()));
                    unsafe_to_release = true;
                    break;
                }
                acquisition_states_[index] = AcquisitionActivationState::Quiesced;
            }

            auto stopped = run_bounded_call(
                "abort-stop",
                [state, index]() { return state->acquisition_instance(index)->stop(); },
                Clock::now() + options_.stop_progress_timeout);
            if (stopped.status != BoundedCallOutcome::Status::Completed) {
                active_->RequestFailure(
                    stopped.status == BoundedCallOutcome::Status::TimedOut
                        ? runtime_error(
                              ErrorCode::Timeout,
                              "Acquisition stop timed out during activation abort")
                        : std::move(stopped.error.value()));
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
            if (closed.status == BoundedCallOutcome::Status::Completed) {
                storage_state_ = StorageActivationState::Closed;
            } else {
                active_->RequestFailure(
                    closed.status == BoundedCallOutcome::Status::TimedOut
                        ? runtime_error(
                              ErrorCode::Timeout, "Storage close timed out during activation abort")
                        : std::move(closed.error.value()));
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
                if (destroyed.status != BoundedCallOutcome::Status::Completed) {
                    active_->RequestFailure(
                        destroyed.status == BoundedCallOutcome::Status::TimedOut
                            ? runtime_error(
                                  ErrorCode::Timeout,
                                  "Acquisition destroy timed out during activation abort")
                            : std::move(destroyed.error.value()));
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
            if (destroyed.status != BoundedCallOutcome::Status::Completed) {
                active_->RequestFailure(
                    destroyed.status == BoundedCallOutcome::Status::TimedOut
                        ? runtime_error(
                              ErrorCode::Timeout,
                              "Storage destroy timed out during activation abort")
                        : std::move(destroyed.error.value()));
                unsafe_to_release = true;
            }
        }
        if (!outside_lifecycle([&] {
                return active_->ShutdownLogging(Clock::now() + options_.logging.shutdown_timeout);
            })) {
            active_->QuarantineLogging("Runtime logging shutdown timed out during abort");
            active_->RecordSecondaryFailure(runtime_error(
                ErrorCode::Timeout, "Runtime logging shutdown timed out during activation abort"));
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
        active_->ReapQuarantinedWorkers();
        if (recovery_abandoned_) {
            return false;
        }
        if (pending_call_ != nullptr && !pending_call_->done.load(std::memory_order_acquire)) {
            return false;
        }
        if (active_->RequiresQuarantine()) {
            return false;
        }
        if (pending_call_ != nullptr && pending_call_->site.find("destroy") != std::string::npos &&
            pending_call_->result->has_value()) {
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
        if (!QuarantineManager::Process().RetainActivation(
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
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::Unavailable, "Runtime stop requires quarantine"));
    }

    void capture_activation_outcome() noexcept {
        if (active_ == nullptr) {
            return;
        }
        try {
            active_->RecordStopLatency();
            last_stats_ = active_->Stats();
            auto payload_stats = active_->PayloadPoolStatistics();
            if (payload_stats.has_value()) {
                last_payload_stats_ = std::move(payload_stats.value());
            }
            auto primary = active_->PrimaryFailure();
            if (primary.has_value()) {
                auto secondary = active_->SecondaryFailures();
                if (secondary.has_value()) {
                    last_secondary_failures_ = std::move(secondary.value());
                }
                last_secondary_failure_overflow_count_ = active_->SecondaryFailureOverflowCount();
                last_primary_failure_ = std::move(primary.value());
            }
        } catch (...) {
        }
    }

    void release_activation_slot() noexcept {
        if (activation_slot_.has_value()) {
            QuarantineManager::Process().ReleaseActivation(*activation_slot_);
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
    std::optional<Error> last_primary_failure_;
    std::vector<Error> last_secondary_failures_;
    std::uint64_t last_secondary_failure_overflow_count_ = 0;
    std::chrono::nanoseconds effective_storage_handoff_timeout_{};
    std::chrono::nanoseconds effective_storage_flush_timeout_{};
    std::chrono::nanoseconds effective_storage_close_timeout_{};
};

Runtime::Runtime(RuntimeConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Runtime::~Runtime() = default;

Result<void> Runtime::Configure() { return impl_->Configure(); }

Result<void> Runtime::Start() { return impl_->Start(); }

Result<void> Runtime::Stop() { return impl_->Stop(); }

RuntimeState Runtime::State() const noexcept { return impl_->State(); }

Result<rodaq_runtime_lifecycle_snapshot> Runtime::LifecycleSnapshot() const {
    return impl_->LifecycleSnapshot();
}

Result<Error> Runtime::PrimaryFailure() const { return impl_->PrimaryFailure(); }

Result<std::vector<Error>> Runtime::SecondaryFailures() const { return impl_->SecondaryFailures(); }

std::uint64_t Runtime::SecondaryFailureOverflowCount() const noexcept {
    return impl_->SecondaryFailureOverflowCount();
}

Result<RuntimeStatsSnapshot> Runtime::Stats() const { return impl_->Stats(); }

Result<PayloadPoolStats> Runtime::PayloadStats() const { return impl_->PayloadStats(); }

}  // namespace rodaq::runtime
