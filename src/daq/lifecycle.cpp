#include "daq/daq_impl.hpp"
#include "config/config_internal.hpp"
#include "plugin/acquisition_plugin_instance.hpp"
#include "plugin/plugin_manager.hpp"
#include "plugin/service_registry.hpp"
#include "plugin/storage_plugin_instance.hpp"
#include <rodaq/plugin/abi_support.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace rodaq {
namespace {
using Clock = std::chrono::steady_clock;
template <typename Config>
Result<void> validate_plugin_options(const Config& options) {
    std::error_code error;
    if (options.path.empty() || !std::filesystem::is_regular_file(options.path, error)) {
        return daq_status(StatusCode::NotFound, "DAQ Plugin path is missing");
    }
    const ConfigNodeView view = options.config.view();
    if (config_abi_view(view).context == nullptr) {
        return daq_status(StatusCode::InvalidArgument, "DAQ Plugin config is empty");
    }
    return {};
}

Result<void> validate_options(const DAQConfig& options) {
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
        return daq_status(StatusCode::InvalidArgument, "DAQ capacity configuration is invalid");
    }
    if (resources.dispatch_capacity != resources.operation_capacity) {
        return daq_status(
            StatusCode::InvalidArgument, "V1 requires equal dispatch and operation capacity");
    }
    if (pool_config.classes.empty()) {
        return daq_status(
            StatusCode::InvalidArgument, "PayloadPool configuration has no size classes");
    }
    std::size_t previous_class_size = 0;
    for (const PayloadClassConfig& payload_class : pool_config.classes) {
        if (payload_class.max_payload_bytes == 0 || payload_class.block_count == 0 ||
            payload_class.max_payload_bytes <= previous_class_size ||
            payload_class.max_payload_bytes >
                std::numeric_limits<std::size_t>::max() / payload_class.block_count) {
            return daq_status(
                StatusCode::InvalidArgument, "PayloadPool size class configuration is invalid");
        }
        previous_class_size = payload_class.max_payload_bytes;
    }
    if (pool_config.classes.back().max_payload_bytes < resources.max_single_payload_bytes) {
        return daq_status(
            StatusCode::InvalidArgument,
            "PayloadPool capacity is below the configured payload limit");
    }
    if (options.acquisition_id.empty() || options.target_uri.empty()) {
        return daq_status(StatusCode::InvalidArgument, "Storage open configuration is empty");
    }
    if (options.acquisitions.empty()) {
        return daq_status(
            StatusCode::InvalidArgument, "At least one Acquisition Plugin is required");
    }
    auto storage = validate_plugin_options(options.storage);
    if (!storage.ok()) {
        return storage;
    }
    for (const AcquisitionPluginConfig& acquisition : options.acquisitions) {
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
    return daq_status(StatusCode::InternalError, std::string(fallback));
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
}  // namespace

Result<void> DAQ::Impl::configure() {
    LifecycleOwner owner(*this);
    if (state_ != DAQState::Constructed) {
        return daq_status(StatusCode::InvalidState, "DAQ cannot be configured from its state");
    }
    auto validated = validate_options(options_);
    if (!validated.ok()) {
        return validated;
    }
    state_ = DAQState::Configured;
    return {};
}

Result<void> DAQ::Impl::start() {
    LifecycleOwner owner(*this);
    if (state_ != DAQState::Configured && state_ != DAQState::Stopped) {
        return daq_status(StatusCode::InvalidState, "DAQ cannot start from its state");
    }
    if (quarantined_ != nullptr) {
        return daq_status(
            StatusCode::Unavailable, "DAQ restart is blocked by quarantined callback state");
    }
    if (activation_generation_ == std::numeric_limits<std::uint32_t>::max()) {
        return daq_status(StatusCode::ResourceExhausted, "DAQ activation generation is exhausted");
    }
    auto activation_slot = plugin::PluginQuarantine::process().reserve_activation("DAQ activation");
    if (!activation_slot.ok()) {
        return std::move(activation_slot).status();
    }
    activation_slot_ = activation_slot.value();
    std::fill(
        acquisition_states_.begin(), acquisition_states_.end(), AcquisitionActivationState::Empty);
    storage_state_ = StorageActivationState::Empty;
    storage_flushed_ = false;
    aborting_ = false;
    ++activation_generation_;
    state_ = DAQState::Starting;
    auto created = ActivationState::create(options_, activation_generation_);
    if (!created.ok()) {
        release_activation_slot();
        state_ = DAQState::Failed;
        return std::move(created).status();
    }
    active_ = std::move(created.value());
    active_->set_failure_signal(failure_signal_);
    if (!failure_worker_.joinable()) {
        try {
            failure_worker_ = std::thread([this] { failure_loop(); });
        } catch (const std::exception& error) {
            active_->request_failure(daq_status(StatusCode::Unavailable, error.what()));
            abort_start();
            return daq_status(StatusCode::Unavailable, error.what());
        }
    }

    auto descriptor_worker = active_->start_descriptor_reconciliation();
    if (!descriptor_worker.ok()) {
        active_->request_failure(descriptor_worker.status());
        abort_start();
        state_ = DAQState::Failed;
        return descriptor_worker;
    }
    auto started = start_activation();
    if (!started.ok()) {
        if (!active_->has_primary_failure()) {
            active_->request_failure(started.status());
        }
        abort_start();
        state_ = DAQState::Failed;
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
    state_ = DAQState::Running;
    return {};
}

Result<void> DAQ::Impl::stop() {
    LifecycleOwner owner(*this);
    return stop_internal();
}

Result<void> DAQ::Impl::stop_internal() {
    if (state_ == DAQState::Stopped) {
        return {};
    }
    if (state_ != DAQState::Running && state_ != DAQState::Failed && state_ != DAQState::Starting &&
        state_ != DAQState::Stopping) {
        return daq_status(StatusCode::InvalidState, "DAQ cannot stop from its state");
    }
    if (active_ == nullptr) {
        if (quarantined_ != nullptr || state_ == DAQState::Failed) {
            return daq_status(StatusCode::Unavailable, "DAQ stop requires quarantine");
        }
        state_ = DAQState::Stopped;
        return {};
    }

    state_ = DAQState::Stopping;
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
        std::shared_ptr<ActivationState> state = active_;
        auto outcome = run_bounded_call(
            "quiesce",
            [state, index]() -> Result<void> {
                auto control = state->acquisition_instance(index)->required_control_interface();
                if (!control.ok()) {
                    return std::move(control).status();
                }
                const auto* table = static_cast<const rodaq_acquisition_control*>(control->vtable);
                rodaq_status error{};
                const rodaq_status_code status = table->quiesce_plugin(control->context, &error);
                if (status != RODAQ_STATUS_OK) {
                    return abi_failure_status(status, error, "Acquisition quiescence failed");
                }
                return {};
            },
            Clock::now() + options_.stop_progress_timeout);
        if (outcome.status == BoundedCallOutcome::Outcome::TimedOut) {
            active_->request_failure(
                daq_status(StatusCode::Timeout, "Acquisition quiescence timed out"));
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
            daq_status(StatusCode::Timeout, "DAQ submission barrier timed out"));
        return quarantine_failure();
    }
    if (!outside_lifecycle([&] {
            return active_->wait_for_admission_drain(Clock::now() + options_.stop_progress_timeout);
        })) {
        active_->request_failure(
            daq_status(StatusCode::Timeout, "DAQ admission barrier timed out"));
        return quarantine_failure();
    }

    StoragePipeline* pipeline = active_->pipeline();
    if (pipeline != nullptr) {
        pipeline->close_dispatch();
        if (!outside_lifecycle([&] {
                return pipeline->wait_dispatch_idle(Clock::now() + options_.stop_progress_timeout);
            })) {
            active_->request_failure(
                daq_status(StatusCode::Timeout, "DAQ dispatch barrier timed out"));
            return quarantine_failure();
        }
        outside_lifecycle([&] { return pipeline->join_dispatch(); });
    }

    if (storage_open_ && !storage_flushed_ && active_->storage_instance().has_value()) {
        std::shared_ptr<ActivationState> state = active_;
        auto flush = run_bounded_call(
            "flush", [state]() { return state->storage_instance()->flush(); },
            Clock::now() + effective_storage_flush_timeout_);
        if (flush.status == BoundedCallOutcome::Outcome::TimedOut) {
            active_->request_failure(daq_status(StatusCode::Timeout, "Storage flush timed out"));
            return quarantine_failure();
        }
        storage_flushed_ = true;
        if (flush.status == BoundedCallOutcome::Outcome::Failed) {
            active_->request_failure(std::move(flush.error));
        }
    }
    if (pipeline != nullptr && !outside_lifecycle([&] {
            return pipeline->wait_operations_idle(Clock::now() + options_.stop_progress_timeout);
        })) {
        active_->request_failure(
            daq_status(StatusCode::Timeout, "DAQ Storage operation barrier timed out"));
    }

    if (storage_open_ && active_->storage_instance().has_value()) {
        const rodaq_storage_close_disposition disposition = active_->state() == DAQState::Failed
                                                                ? RODAQ_STORAGE_CLOSE_FAILURE
                                                                : RODAQ_STORAGE_CLOSE_NORMAL;
        std::shared_ptr<ActivationState> state = active_;
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
            active_->request_failure(daq_status(StatusCode::Timeout, "Storage close timed out"));
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
            !outside_lifecycle([&] { return pipeline->wait_operations_idle(release_deadline); })) {
            if (!pipeline->retain_blocked_release_workers()) {
                active_->record_secondary_failure(daq_status(
                    StatusCode::ResourceExhausted,
                    "DAQ release worker quarantine capacity is exhausted"));
            }
            active_->request_failure(daq_status(
                StatusCode::Timeout, "DAQ callback/release quiescence barrier timed out"));
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
        std::shared_ptr<ActivationState> state = active_;
        auto stopped = run_bounded_call(
            "stop", [state, index]() { return state->acquisition_instance(index)->stop(); },
            Clock::now() + options_.stop_progress_timeout);
        if (stopped.status == BoundedCallOutcome::Outcome::TimedOut) {
            active_->request_failure(daq_status(StatusCode::Timeout, "Acquisition stop timed out"));
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
        std::shared_ptr<ActivationState> state = active_;
        auto destroyed = run_bounded_call(
            "destroy-acquisition",
            [state, index]() {
                state->acquisition_instance(index)->destroy();
                return Result<void>();
            },
            Clock::now() + options_.stop_progress_timeout);
        if (destroyed.status == BoundedCallOutcome::Outcome::TimedOut) {
            active_->request_failure(
                daq_status(StatusCode::Timeout, "Acquisition destroy timed out"));
            return quarantine_failure();
        }
        if (destroyed.status == BoundedCallOutcome::Outcome::Failed) {
            active_->request_failure(std::move(destroyed.error));
            return quarantine_failure();
        }
    }
    if (active_->storage_instance().has_value() && active_->storage_instance()->valid()) {
        std::shared_ptr<ActivationState> state = active_;
        auto destroyed = run_bounded_call(
            "destroy-storage",
            [state]() {
                state->storage_instance()->destroy();
                return Result<void>();
            },
            Clock::now() + effective_storage_close_timeout_);
        if (destroyed.status == BoundedCallOutcome::Outcome::TimedOut) {
            active_->request_failure(daq_status(StatusCode::Timeout, "Storage destroy timed out"));
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
        active_->quarantine_logging("DAQ logging shutdown timed out");
        active_->record_secondary_failure(
            daq_status(StatusCode::Timeout, "DAQ logging shutdown timed out"));
    }

    const bool failed = active_->state() == DAQState::Failed || !active_->primary_failure().ok();
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
    state_ = failed ? DAQState::Failed : DAQState::Stopped;
    if (failed) {
        state_ = DAQState::Failed;
    }
    if (failed) {
        return daq_status(StatusCode::Unavailable, "DAQ stopped after failure");
    }
    return {};
}

Result<void> DAQ::Impl::start_activation() {
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
    std::shared_ptr<ActivationState> state = active_;
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
        return state->storage_instance()->configure(config_abi_view(storage_config.view()));
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
        capabilities->full_fidelity != RODAQ_TRUE || capabilities->maximum_inflight_records == 0 ||
        capabilities->maximum_inflight_bytes == 0) {
        return daq_status(StatusCode::InvalidArgument, "Storage capabilities are invalid");
    }
    std::size_t effective_payload_limit = options_.resources.max_single_payload_bytes;
    if (capabilities->maximum_payload_bytes != 0) {
        effective_payload_limit =
            std::min(effective_payload_limit, bounded_size(capabilities->maximum_payload_bytes));
    }
    effective_payload_limit =
        std::min(effective_payload_limit, bounded_size(capabilities->maximum_inflight_bytes));
    if (effective_payload_limit == 0) {
        return daq_status(StatusCode::InvalidArgument, "Effective Storage payload limit is zero");
    }

    const std::size_t effective_capacity = std::min(
        {options_.resources.operation_capacity, options_.resources.completion_capacity,
         bounded_size(capabilities->maximum_inflight_records)});
    if (effective_capacity == 0) {
        return daq_status(StatusCode::InvalidArgument, "Effective Storage capacity is zero");
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
        const DAQConfig& active_options = state->options();
        rodaq_storage_open_request open_request{};
        open_request.struct_size = sizeof(open_request);
        open_request.session_id = rodaq_string_view{
            active_options.acquisition_id.data(), active_options.acquisition_id.size()};
        open_request.target_uri =
            rodaq_string_view{active_options.target_uri.data(), active_options.target_uri.size()};
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
        return daq_status(StatusCode::InternalError, "Acquisition count changed during activation");
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
                    config_abi_view(acquisition_config.view()));
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

}  // namespace rodaq
