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

Result<void> validate_plugin_options(const RuntimePluginOptions& options) {
    std::error_code error;
    if (options.path.empty() || !std::filesystem::is_regular_file(options.path, error)) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::NotFound, "Runtime Plugin path is missing"));
    }
    const plugin::ConfigView view = options.config.view();
    if (view.AbiView().context == nullptr) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Runtime Plugin config is empty"));
    }
    return {};
}

Result<void> validate_options(const RuntimeOptions& options) {
    if (options.max_sources == 0 || options.max_clocks == 0 || options.max_schemas == 0 ||
        options.max_timestamp_key_sets == 0 || options.max_streams == 0 ||
        options.max_stream_descriptors == 0 || options.max_bindings == 0 ||
        options.structural_arena_bytes == 0 || options.payload_block_bytes == 0 ||
        options.payload_block_count == 0 || options.max_single_payload_bytes == 0 ||
        options.dispatch_capacity == 0 || options.operation_capacity == 0 ||
        options.completion_capacity == 0 || options.submit_wait_timeout.count() <= 0 ||
        options.stop_progress_timeout.count() <= 0) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Runtime capacity configuration is invalid"));
    }
    if (options.dispatch_capacity != options.operation_capacity) {
        return nonstd::make_unexpected(runtime_error(
            ErrorCode::InvalidArgument, "V1 requires equal dispatch and operation capacity"));
    }
    if (options.payload_block_count < options.operation_capacity ||
        options.payload_block_bytes < options.max_single_payload_bytes) {
        return nonstd::make_unexpected(runtime_error(
            ErrorCode::InvalidArgument,
            "PayloadPool capacity is below the configured record requirements"));
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
    for (const RuntimePluginOptions& acquisition : options.acquisitions) {
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
BoundedCallOutcome run_bounded_call(
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
public:
    explicit Impl(RuntimeOptions options)
    : options_(std::move(options)),
      acquisition_states_(options_.acquisitions.size(), AcquisitionActivationState::Empty) {}

    Result<void> Configure() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
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
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (state_ != RuntimeState::Configured && state_ != RuntimeState::Stopped) {
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::InvalidState, "Runtime cannot start from its state"));
        }
        if (quarantined_ != nullptr) {
            return nonstd::make_unexpected(runtime_error(
                ErrorCode::Unavailable,
                "Runtime restart is blocked by quarantined callback state"));
        }
        if (activation_generation_ == std::numeric_limits<std::uint64_t>::max()) {
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
        ++activation_generation_;
        state_ = RuntimeState::Starting;
        auto created = ActiveRuntimeState::Create(options_, activation_generation_);
        if (!created.has_value()) {
            release_activation_slot();
            state_ = RuntimeState::Failed;
            return nonstd::make_unexpected(std::move(created.error()));
        }
        active_ = std::move(created.value());

        auto started = start_activation();
        if (!started.has_value()) {
            active_->RequestFailure(started.error());
            abort_start();
            state_ = RuntimeState::Failed;
            return started;
        }
        active_->MarkRunning();
        state_ = RuntimeState::Running;
        return {};
    }

    Result<void> Stop() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
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

        for (std::size_t index = 0; index < acquisition_states_.size(); ++index) {
            if (acquisition_states_[index] != AcquisitionActivationState::Started ||
                !active_->acquisition_instance(index).has_value()) {
                continue;
            }
            std::shared_ptr<ActiveRuntimeState> state = active_;
            auto outcome = run_bounded_call(
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

        if (!active_->WaitForSubmissions(Clock::now() + options_.stop_progress_timeout)) {
            active_->RequestFailure(
                runtime_error(ErrorCode::Timeout, "Runtime submission barrier timed out"));
            return quarantine_failure();
        }
        if (!active_->WaitForAdmissionDrain(Clock::now() + options_.stop_progress_timeout)) {
            active_->RequestFailure(
                runtime_error(ErrorCode::Timeout, "Runtime admission barrier timed out"));
            return quarantine_failure();
        }

        StoragePipeline* pipeline = active_->pipeline();
        if (pipeline != nullptr) {
            pipeline->CloseDispatch();
            if (!pipeline->WaitDispatchIdle(Clock::now() + options_.stop_progress_timeout)) {
                active_->RequestFailure(
                    runtime_error(ErrorCode::Timeout, "Runtime dispatch barrier timed out"));
                return quarantine_failure();
            }
            pipeline->JoinDispatch();
        }

        if (storage_open_ && active_->storage_instance().has_value()) {
            std::shared_ptr<ActiveRuntimeState> state = active_;
            auto flush = run_bounded_call(
                [state]() { return state->storage_instance()->flush(); },
                Clock::now() + effective_storage_flush_timeout_);
            if (flush.status == BoundedCallOutcome::Status::TimedOut) {
                active_->RequestFailure(
                    runtime_error(ErrorCode::Timeout, "Storage flush timed out"));
                return quarantine_failure();
            }
            if (flush.status == BoundedCallOutcome::Status::Failed) {
                active_->RequestFailure(std::move(flush.error.value()));
            }
        }
        if (pipeline != nullptr &&
            !pipeline->WaitOperationsIdle(Clock::now() + options_.stop_progress_timeout)) {
            active_->RequestFailure(
                runtime_error(ErrorCode::Timeout, "Runtime Storage operation barrier timed out"));
        }

        if (storage_open_ && active_->storage_instance().has_value()) {
            const rodaq_storage_close_disposition disposition =
                active_->state() == RuntimeState::Failed ? RODAQ_STORAGE_CLOSE_FAILURE
                                                         : RODAQ_STORAGE_CLOSE_NORMAL;
            std::shared_ptr<ActiveRuntimeState> state = active_;
            auto close = run_bounded_call(
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
            pipeline->CloseCallbacks();
            pipeline->JoinCompletion();
            if (!pipeline->WaitOperationsIdle(Clock::now() + options_.stop_progress_timeout)) {
                active_->RequestFailure(runtime_error(
                    ErrorCode::Timeout, "Runtime callback quiescence barrier timed out"));
            }
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
                [state, index]() { return state->acquisition_instance(index)->stop(); },
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

        const bool failed =
            active_->state() == RuntimeState::Failed || active_->PrimaryFailure().has_value();
        last_stats_ = active_->Stats();
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

    ~Impl() {
        if (active_ != nullptr) {
            static_cast<void>(Stop());
        }
        if (quarantined_ != nullptr) {
            if (!activation_slot_.has_value()) {
                std::abort();
            }
            try {
                if (!QuarantineManager::Process().RetainActivation(
                        *activation_slot_, quarantined_, "Runtime activation quarantine")) {
                    std::abort();
                }
            } catch (...) {
                std::abort();
            }
            quarantined_.reset();
            activation_slot_.reset();
        } else {
            release_activation_slot();
        }
    }

private:
    template <typename Callable>
    Result<void> supervise_start_call(Callable callable) {
        auto outcome =
            run_bounded_call(std::move(callable), Clock::now() + options_.stop_progress_timeout);
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
        std::size_t effective_payload_limit = options_.max_single_payload_bytes;
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
            {options_.operation_capacity, options_.completion_capacity,
             bounded_size(capabilities->maximum_inflight_records), options_.payload_block_count});
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

        active_->options().max_single_payload_bytes = effective_payload_limit;
        active_->options().dispatch_capacity = effective_capacity;
        active_->options().operation_capacity = effective_capacity;
        active_->options().completion_capacity = effective_completion_capacity;
        active_->SetDescriptorProgressTimeout(effective_descriptor_timeout);

        auto opened = supervise_start_call([state]() {
            const RuntimeOptions& active_options = state->options();
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
        return {};
    }

    void abort_start() noexcept {
        if (active_ == nullptr) {
            return;
        }
        active_->MarkStopping();
        if (start_quarantine_required_) {
            capture_activation_outcome();
            quarantined_ = std::move(active_);
            std::fill(
                acquisition_states_.begin(), acquisition_states_.end(),
                AcquisitionActivationState::Quarantined);
            storage_state_ = StorageActivationState::Quarantined;
            storage_open_ = false;
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
                 activation_state != AcquisitionActivationState::Started)) {
                continue;
            }

            auto quiesced = run_bounded_call(
                [state, index]() -> Result<void> {
                    auto control = state->acquisition_instance(index)->required_control_interface();
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
                                     status, error, "Acquisition quiescence failed during abort"));
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

            auto stopped = run_bounded_call(
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
        storage_open_ = false;
        start_quarantine_required_ = false;
        state_ = RuntimeState::Failed;
    }

    Result<void> quarantine_failure() {
        if (active_ != nullptr) {
            quarantined_ = std::move(active_);
        }
        state_ = RuntimeState::Failed;
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::Unavailable, "Runtime stop requires quarantine"));
    }

    void capture_activation_outcome() noexcept {
        if (active_ == nullptr) {
            return;
        }
        try {
            last_stats_ = active_->Stats();
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

    RuntimeOptions options_;
    mutable std::mutex lifecycle_mutex_;
    RuntimeState state_ = RuntimeState::Constructed;
    std::uint64_t activation_generation_ = 0;
    std::shared_ptr<ActiveRuntimeState> active_;
    std::shared_ptr<ActiveRuntimeState> quarantined_;
    std::optional<std::uint64_t> activation_slot_;
    std::vector<AcquisitionActivationState> acquisition_states_;
    StorageActivationState storage_state_ = StorageActivationState::Empty;
    bool storage_open_ = false;
    bool start_quarantine_required_ = false;
    std::optional<RuntimeStatsSnapshot> last_stats_;
    std::optional<Error> last_primary_failure_;
    std::vector<Error> last_secondary_failures_;
    std::uint64_t last_secondary_failure_overflow_count_ = 0;
    std::chrono::nanoseconds effective_storage_handoff_timeout_{};
    std::chrono::nanoseconds effective_storage_flush_timeout_{};
    std::chrono::nanoseconds effective_storage_close_timeout_{};
};

Runtime::Runtime(RuntimeOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

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

}  // namespace rodaq::runtime
