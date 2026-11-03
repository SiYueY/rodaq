#include "runtime/runtime.hpp"

#include "plugin/acquisition_plugin_instance.hpp"
#include "plugin/plugin_manager.hpp"
#include "plugin/service_registry.hpp"
#include "plugin/storage_plugin_instance.hpp"
#include "runtime/active_runtime_state.hpp"

#include <rodaq/plugin/abi/acquisition.h>
#include <rodaq/plugin/abi_support.hpp>
#include <rodaq/plugin/abi/storage.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
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

Result<void> query_storage_control(
    plugin::StoragePluginInstance& instance, ActiveRuntimeState& state) {
    auto control = instance.required_control_interface();
    if (!control.has_value()) {
        return nonstd::make_unexpected(std::move(control.error()));
    }
    state.SetStorageControl(control.value());
    return {};
}

Result<void> query_acquisition_control(
    std::size_t index, plugin::AcquisitionPluginInstance& instance, ActiveRuntimeState& state) {
    auto control = instance.required_control_interface();
    if (!control.has_value()) {
        return nonstd::make_unexpected(std::move(control.error()));
    }
    state.SetAcquisitionControl(index, control.value());
    return {};
}

Error abi_error(rodaq_status status, const rodaq_error& error, std::string_view fallback) {
    auto converted = result_from_status(status, &error, "runtime");
    if (!converted.has_value()) {
        return std::move(converted.error());
    }
    return runtime_error(ErrorCode::InternalError, std::string(fallback));
}

}  // namespace

class Runtime::Impl final {
public:
    explicit Impl(RuntimeOptions options)
    : options_(std::move(options)), acquisition_started_(options_.acquisitions.size(), false) {}

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
        ++activation_generation_;
        state_ = RuntimeState::Starting;
        auto created = ActiveRuntimeState::Create(options_, activation_generation_);
        if (!created.has_value()) {
            state_ = RuntimeState::Failed;
            return nonstd::make_unexpected(std::move(created.error()));
        }
        active_ = std::move(created.value());

        auto started = start_activation();
        if (!started.has_value()) {
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
            state_ = RuntimeState::Stopped;
            return {};
        }

        state_ = RuntimeState::Stopping;
        active_->MarkStopping();
        const auto deadline = Clock::now() + options_.stop_progress_timeout;

        quiesce_acquisition_plugins();
        if (!active_->WaitForSubmissions(deadline)) {
            active_->RequestFailure(
                runtime_error(ErrorCode::Timeout, "Runtime submission barrier timed out"));
        }
        if (!active_->WaitForAdmissionDrain(deadline)) {
            active_->RequestFailure(
                runtime_error(ErrorCode::Timeout, "Runtime admission barrier timed out"));
        }

        StoragePipeline* pipeline = active_->pipeline();
        if (pipeline != nullptr) {
            pipeline->CloseDispatch();
            if (!pipeline->WaitDispatchIdle(deadline)) {
                active_->RequestFailure(
                    runtime_error(ErrorCode::Timeout, "Runtime dispatch barrier timed out"));
            }
            pipeline->JoinDispatch();
        }

        flush_storage();
        if (pipeline != nullptr && !pipeline->WaitOperationsIdle(deadline)) {
            active_->RequestFailure(
                runtime_error(ErrorCode::Timeout, "Runtime Storage operation barrier timed out"));
        }

        close_storage();
        if (pipeline != nullptr) {
            pipeline->CloseCallbacks();
            pipeline->JoinCompletion();
            if (!pipeline->WaitOperationsIdle(Clock::now() + options_.stop_progress_timeout)) {
                active_->RequestFailure(runtime_error(
                    ErrorCode::Timeout, "Runtime callback quiescence barrier timed out"));
            }
        }

        stop_acquisition_plugins();
        const bool unresolved = pipeline != nullptr && pipeline->HasUnresolvedOperations();
        if (!unresolved) {
            for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
                active_->acquisition_instance(index).reset();
            }
            active_->storage_instance().reset();
        }
        const bool failed =
            active_->state() == RuntimeState::Failed || active_->PrimaryFailure().has_value();
        last_stats_ = active_->Stats();
        auto primary_failure = active_->PrimaryFailure();
        if (primary_failure.has_value()) {
            last_primary_failure_ = std::move(primary_failure.value());
        }

        if (unresolved) {
            quarantined_ = std::move(active_);
            state_ = RuntimeState::Failed;
        } else {
            active_.reset();
            state_ = failed ? RuntimeState::Failed : RuntimeState::Stopped;
        }
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

    std::uint64_t SecondaryFailureOverflowCount() const noexcept {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_ != nullptr) {
            return active_->SecondaryFailureOverflowCount();
        }
        return quarantined_ == nullptr ? 0 : quarantined_->SecondaryFailureOverflowCount();
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
            static_cast<void>(quarantined_.release());
        }
    }

private:
    Result<void> start_activation() {
        plugin::PluginManager manager;

        auto storage_library = manager.load_library(options_.storage.path);
        if (!storage_library.has_value()) {
            return nonstd::make_unexpected(std::move(storage_library.error()));
        }
        auto storage = manager.create_storage_plugin_instance(std::move(storage_library.value()));
        if (!storage.has_value()) {
            return nonstd::make_unexpected(std::move(storage.error()));
        }
        active_->storage_instance().emplace(std::move(storage.value()));

        plugin::ServiceRegistry storage_registry(6, 128);
        auto registered = active_->RegisterStorageServices(storage_registry);
        if (!registered.has_value()) {
            return registered;
        }
        auto frozen = storage_registry.Freeze();
        if (!frozen.has_value()) {
            return frozen;
        }
        auto bound = active_->storage_instance()->bind(storage_registry.AbiView());
        if (!bound.has_value()) {
            return bound;
        }
        auto control = query_storage_control(active_->storage_instance().value(), *active_);
        if (!control.has_value()) {
            return control;
        }
        auto configured =
            active_->storage_instance()->configure(options_.storage.config.view().AbiView());
        if (!configured.has_value()) {
            return configured;
        }
        auto capabilities = active_->StorageCapabilities();
        if (!capabilities.has_value()) {
            return nonstd::make_unexpected(std::move(capabilities.error()));
        }
        if (capabilities->maximum_payload_bytes == 0) {
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::InvalidArgument, "Storage payload limit is zero"));
        }
        const std::size_t effective_payload_limit =
            capabilities->maximum_payload_bytes < options_.max_single_payload_bytes
                ? static_cast<std::size_t>(capabilities->maximum_payload_bytes)
                : options_.max_single_payload_bytes;
        options_.max_single_payload_bytes = effective_payload_limit;
        active_->options().max_single_payload_bytes = effective_payload_limit;

        rodaq_storage_open_request open_request{};
        open_request.struct_size = sizeof(open_request);
        open_request.session_id =
            rodaq_string_view{options_.session_id.data(), options_.session_id.size()};
        open_request.target_uri =
            rodaq_string_view{options_.target_uri.data(), options_.target_uri.size()};
        auto opened = active_->storage_instance()->open(open_request);
        if (!opened.has_value()) {
            return opened;
        }
        storage_open_ = true;

        auto pipeline = active_->InitializePipeline(
            effective_payload_limit, &active_->storage_instance().value(),
            options_.stop_progress_timeout);
        if (!pipeline.has_value()) {
            return pipeline;
        }

        if (options_.acquisitions.size() != active_->options().acquisitions.size()) {
            return nonstd::make_unexpected(runtime_error(
                ErrorCode::InternalError, "Acquisition count changed during activation"));
        }
        for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
            auto library = manager.load_library(options_.acquisitions[index].path);
            if (!library.has_value()) {
                return nonstd::make_unexpected(std::move(library.error()));
            }
            auto instance = manager.create_acquisition_plugin_instance(std::move(library.value()));
            if (!instance.has_value()) {
                return nonstd::make_unexpected(std::move(instance.error()));
            }
            active_->acquisition_instance(index).emplace(std::move(instance.value()));

            plugin::ServiceRegistry registry(4, 128);
            auto acquisition_registered = active_->RegisterAcquisitionServices(registry, index);
            if (!acquisition_registered.has_value()) {
                return acquisition_registered;
            }
            auto acquisition_frozen = registry.Freeze();
            if (!acquisition_frozen.has_value()) {
                return acquisition_frozen;
            }
            auto acquisition_bound = active_->acquisition_instance(index)->bind(registry.AbiView());
            if (!acquisition_bound.has_value()) {
                return acquisition_bound;
            }
            auto acquisition_control = query_acquisition_control(
                index, active_->acquisition_instance(index).value(), *active_);
            if (!acquisition_control.has_value()) {
                return acquisition_control;
            }
            auto acquisition_configured = active_->acquisition_instance(index)->configure(
                options_.acquisitions[index].config.view().AbiView());
            if (!acquisition_configured.has_value()) {
                return acquisition_configured;
            }
        }

        for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
            auto started = active_->acquisition_instance(index)->start();
            if (!started.has_value()) {
                return started;
            }
            acquisition_started_[index] = true;
        }
        return {};
    }

    void abort_start() noexcept {
        if (active_ == nullptr) {
            return;
        }
        active_->MarkStopping();
        if (active_->pipeline() != nullptr) {
            active_->pipeline()->CloseAdmission();
            active_->pipeline()->CloseDispatch();
            active_->pipeline()->JoinDispatch();
            active_->pipeline()->CloseCallbacks();
            active_->pipeline()->JoinCompletion();
        }
        for (std::size_t index = 0; index < acquisition_started_.size(); ++index) {
            if (acquisition_started_[index] && active_->acquisition_instance(index).has_value()) {
                static_cast<void>(active_->acquisition_instance(index)->stop());
            }
        }
        if (storage_open_ && active_->storage_instance().has_value()) {
            rodaq_storage_close_request close_request{};
            close_request.struct_size = sizeof(close_request);
            close_request.disposition = RODAQ_STORAGE_CLOSE_ACTIVATION_ABORT;
            static_cast<void>(active_->storage_instance()->close(close_request));
        }
        active_.reset();
        acquisition_started_.assign(options_.acquisitions.size(), false);
        storage_open_ = false;
        state_ = RuntimeState::Failed;
    }

    void quiesce_acquisition_plugins() noexcept {
        for (std::size_t index = 0; index < acquisition_started_.size(); ++index) {
            if (!acquisition_started_[index] || !active_->acquisition_instance(index).has_value()) {
                continue;
            }
            auto control = active_->acquisition_instance(index)->required_control_interface();
            if (!control.has_value()) {
                active_->RequestFailure(std::move(control.error()));
                continue;
            }
            const auto* table = static_cast<const rodaq_acquisition_control_v1*>(control->vtable);
            rodaq_error error{};
            const rodaq_status status = table->quiesce_plugin(control->context, &error);
            if (status != RODAQ_STATUS_OK) {
                active_->RequestFailure(abi_error(status, error, "Acquisition quiescence failed"));
            }
        }
    }

    void flush_storage() noexcept {
        if (!storage_open_ || !active_->storage_instance().has_value()) {
            return;
        }
        auto flushed = active_->storage_instance()->flush();
        if (!flushed.has_value()) {
            active_->RequestFailure(flushed.error());
        }
    }

    void close_storage() noexcept {
        if (!storage_open_ || !active_->storage_instance().has_value()) {
            return;
        }
        rodaq_storage_close_request request{};
        request.struct_size = sizeof(request);
        request.disposition = active_->state() == RuntimeState::Failed ? RODAQ_STORAGE_CLOSE_FAILURE
                                                                       : RODAQ_STORAGE_CLOSE_NORMAL;
        auto closed = active_->storage_instance()->close(request);
        storage_open_ = false;
        if (!closed.has_value()) {
            active_->RequestFailure(closed.error());
        }
    }

    void stop_acquisition_plugins() noexcept {
        for (std::size_t index = 0; index < acquisition_started_.size(); ++index) {
            if (!acquisition_started_[index] || !active_->acquisition_instance(index).has_value()) {
                continue;
            }
            auto stopped = active_->acquisition_instance(index)->stop();
            if (!stopped.has_value()) {
                active_->RequestFailure(stopped.error());
            }
            acquisition_started_[index] = false;
        }
    }

    RuntimeOptions options_;
    mutable std::mutex lifecycle_mutex_;
    RuntimeState state_ = RuntimeState::Constructed;
    std::uint64_t activation_generation_ = 0;
    std::unique_ptr<ActiveRuntimeState> active_;
    std::unique_ptr<ActiveRuntimeState> quarantined_;
    std::vector<bool> acquisition_started_;
    bool storage_open_ = false;
    std::optional<RuntimeStatsSnapshot> last_stats_;
    std::optional<Error> last_primary_failure_;
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

std::uint64_t Runtime::SecondaryFailureOverflowCount() const noexcept {
    return impl_->SecondaryFailureOverflowCount();
}

Result<RuntimeStatsSnapshot> Runtime::Stats() const { return impl_->Stats(); }

}  // namespace rodaq::runtime
