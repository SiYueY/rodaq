#include "activation/activation_internal.hpp"
#include "plugin/plugin_status.hpp"
#include "plugin/plugin_quarantine.hpp"
#include "logging/spdlog_backend.hpp"

#include <rodaq/plugin/abi_support.hpp>
#include <rodaq/plugin/services.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rodaq {

Result<std::unique_ptr<ActivationState>> ActivationState::create(
    DAQConfig config, std::uint64_t activation_generation) {
    try {
        return std::unique_ptr<ActivationState>(
            new ActivationState(std::move(config), activation_generation));
    } catch (const std::bad_alloc&) {
        return activation_status(StatusCode::ResourceExhausted, "DAQ activation allocation failed");
    } catch (const std::exception& error) {
        return activation_status(StatusCode::InternalError, error.what());
    } catch (...) {
        return activation_status(StatusCode::InternalError, "DAQ activation construction failed");
    }
}

ActivationState::ActivationState(DAQConfig config, std::uint64_t activation_generation)
: options_(std::move(config)),
  activation_generation_(activation_generation),
  arena_([this] {
      auto result = StructuralArena::create(options_.resources.structural_arena_bytes);
      if (!result.ok()) {
          throw std::runtime_error(std::string(result.status().message()));
      }
      return std::move(result.value());
  }()),
  sources_(make_registry<SourceEntry, SourceHandle>(
      options_.resources.max_sources, registry_generation(activation_generation))),
  clocks_(make_registry<ClockEntry, ClockHandle>(
      options_.resources.max_clocks, registry_generation(activation_generation))),
  schemas_(make_registry<SchemaEntry, SchemaHandle>(
      options_.resources.max_schemas, registry_generation(activation_generation))),
  timestamp_key_sets_(make_registry<TimestampKeySetEntry, TimestampKeySetHandle>(
      options_.resources.max_timestamp_key_sets, registry_generation(activation_generation))),
  streams_(make_registry<StreamEntry, StreamHandle>(
      options_.resources.max_streams, registry_generation(activation_generation))),
  descriptors_(make_registry<StreamDescriptorEntry, StreamDescriptorHandle>(
      options_.resources.max_stream_descriptors, registry_generation(activation_generation))),
  bindings_(make_registry<BindingEntry, BindingHandle>(
      options_.resources.max_bindings, registry_generation(activation_generation))),
  sequences_([this] {
      auto result = AcceptanceSequenceAllocator::create(options_.resources.max_streams);
      if (!result.ok()) {
          throw std::runtime_error(std::string(result.status().message()));
      }
      return std::move(result.value());
  }()),
  payload_pool_([this] {
      auto result = PayloadPool::create(options_.resources.payload_pool);
      if (!result.ok()) {
          throw std::runtime_error(std::string(result.status().message()));
      }
      return std::move(result.value());
  }()),
  projections_(ProjectionLimits{128, 8, 1024U * 1024U, 8U * 1024U * 1024U}),
  state_(DAQState::Starting),
  admission_wait_latency_(make_latency_histogram()),
  storage_handoff_latency_(make_latency_histogram()),
  payload_consumed_latency_(make_latency_histogram()),
  persistence_latency_(make_latency_histogram()),
  stop_latency_(make_latency_histogram()) {
    source_handles_.reserve(options_.resources.max_sources);
    clock_handles_.reserve(options_.resources.max_clocks);
    schema_handles_.reserve(options_.resources.max_schemas);
    timestamp_key_set_handles_.reserve(options_.resources.max_timestamp_key_sets);
    stream_handles_.reserve(options_.resources.max_streams);
    descriptor_handles_.reserve(options_.resources.max_stream_descriptors);
    binding_handles_.reserve(options_.resources.max_bindings);
    secondary_failures_.reserve(kMaxSecondaryFailures);
    descriptor_worker_requests_.resize(options_.resources.max_stream_descriptors);
    acquisition_controls_.resize(options_.acquisitions.size());
    acquisition_instances_.resize(options_.acquisitions.size());
    acquisition_health_.reserve(options_.acquisitions.size());
    for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
        acquisition_health_.push_back(
            std::make_unique<std::atomic<std::uint32_t>>(RODAQ_ACQUISITION_PLUGIN_AVAILABLE));
    }
    acquisition_contexts_.reserve(options_.acquisitions.size());
    for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
        auto context = std::make_unique<plugin::AcquisitionServiceContext>();
        context->state = this;
        context->plugin_index = index;
        acquisition_contexts_.push_back(std::move(context));
    }
    storage_context_ = std::make_unique<plugin::StorageServiceContext>();
    storage_context_->state = this;
    auto logging = LoggingContext::create(options_.logging, std::make_shared<SpdlogBackend>());
    if (!logging.ok()) {
        throw std::runtime_error(std::string(logging.status().message()));
    }
    logging_context_ = std::move(logging.value());
    auto ingress = initialize_ingress_clock();
    if (!ingress.ok()) {
        throw std::runtime_error(std::string(ingress.status().message()));
    }
    projections_.set_registration_sink(
        [this](rodaq_stream_descriptor_handle descriptor, std::string_view projection_id) {
            on_projection_registered(descriptor, projection_id);
        });
}

ActivationState::~ActivationState() noexcept {
    payload_pool_.set_capacity_changed_sink({});
    if (!descriptor_worker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
        descriptor_worker_closing_ = true;
        descriptor_worker_count_ = 0;
        descriptor_worker_condition_.notify_all();
    }
    descriptor_worker_.join();
}

void ActivationState::set_acquisition_control(std::size_t plugin_index, rodaq_interface control) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (plugin_index >= acquisition_controls_.size() || control.context == nullptr ||
        control.vtable == nullptr) {
        throw std::invalid_argument("Acquisition control interface is invalid");
    }
    acquisition_controls_[plugin_index] = control;
}

void ActivationState::set_storage_control(rodaq_interface control) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (control.context == nullptr || control.vtable == nullptr) {
        throw std::invalid_argument("Storage control interface is invalid");
    }
    storage_control_ = control;
}

Result<rodaq_storage_capabilities> ActivationState::storage_capabilities() const {
    if (!storage_control_.has_value()) {
        return activation_status(
            StatusCode::InvalidState, "Storage control interface is unavailable");
    }
    const auto* table = static_cast<const rodaq_storage_control*>(storage_control_->vtable);
    if (table->get_capabilities == nullptr) {
        return activation_status(
            StatusCode::InvalidArgument, "Storage capabilities slot is missing");
    }
    rodaq_storage_capabilities capabilities{};
    capabilities.struct_size = sizeof(capabilities);
    rodaq_status error{};
    const rodaq_status_code status =
        table->get_capabilities(storage_control_->context, &capabilities, &error);
    if (status != RODAQ_STATUS_OK) {
        auto converted = status_from_abi(status, &error, "storage");
        return converted.ok()
                   ? activation_status(StatusCode::InternalError, "Storage capabilities failed")
                   : std::move(converted);
    }
    if (capabilities.struct_size < RODAQ_STORAGE_CAPABILITIES_MIN_STRUCT_SIZE) {
        return activation_status(
            StatusCode::InvalidArgument, "Storage capabilities output is incomplete");
    }
    return capabilities;
}

void ActivationState::set_descriptor_progress_timeout(std::chrono::nanoseconds timeout) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    descriptor_progress_timeout_ = timeout;
}

void ActivationState::set_binding_progress_timeout(std::chrono::nanoseconds timeout) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    binding_progress_timeout_ = timeout;
}

std::shared_ptr<void> ActivationState::track_control_call() {
    auto state = shared_from_this();
    // shared_ptr invokes its deleter if control-block allocation fails too.
    // Establish the count first so that path also has a matching decrement.
    active_control_calls_.fetch_add(1, std::memory_order_acq_rel);
    return std::shared_ptr<void>(state.get(), [state](void*) {
        state->active_control_calls_.fetch_sub(1, std::memory_order_acq_rel);
    });
}

void ActivationState::reap_quarantined_workers() noexcept {
    bool descriptor_done = false;
    {
        std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
        descriptor_done = !descriptor_worker_started_;
    }
    if (descriptor_done && descriptor_worker_.joinable()) {
        descriptor_worker_.join();
    }
    if (pipeline_ != nullptr) {
        pipeline_->reap_completed_workers();
    }
}

Result<void> ActivationState::initialize_pipeline(
    std::size_t effective_payload_limit, std::size_t maximum_payload_inflight_bytes,
    plugin::StoragePluginInstance* storage, std::chrono::nanoseconds progress_timeout) {
    if (pipeline_ != nullptr) {
        return activation_status(StatusCode::InvalidState, "DAQ pipeline is already initialized");
    }
    const PayloadPoolConfig& pool_config = options_.resources.payload_pool;
    if (effective_payload_limit == 0 || pool_config.classes.empty() ||
        pool_config.classes.back().max_payload_bytes < effective_payload_limit) {
        return activation_status(
            StatusCode::InvalidArgument,
            "Configured payload block size is below the effective payload limit");
    }
    try {
        pipeline_ = std::make_unique<StoragePipeline>(
            options_.resources.operation_capacity, options_.resources.completion_capacity,
            effective_payload_limit, maximum_payload_inflight_bytes, storage, progress_timeout,
            options_.resources.payload_release_workers,
            [this](Status error) { request_failure(std::move(error)); },
            [this](std::size_t bytes) noexcept {
                if (!stats_.record_payload_released(bytes)) {
                    request_failure(activation_status(
                        StatusCode::InternalError, "DAQ inflight byte accounting underflowed"));
                }
            },
            [this](rodaq_storage_persistence_state persistence) noexcept {
                stats_.record_persistence(persistence);
                if (!stats_.record_operation_completed()) {
                    request_failure(activation_status(
                        StatusCode::InternalError, "DAQ inflight record accounting underflowed"));
                }
            },
            [this](StorageLatencyKind kind, std::int64_t duration_ns) noexcept {
                record_storage_latency(kind, duration_ns);
            },
            [this](
                const AcceptedRecord& record, rodaq_storage_operation operation,
                std::array<rodaq_timestamp_value, TimestampState::kMaxSlots>& values) {
                const BindingEntry* binding = bindings_.find_stable(record.binding());
                if (binding == nullptr) {
                    return Result<rodaq_storage_submission>(activation_status(
                        StatusCode::InvalidState, "Record Binding is unavailable"));
                }
                const StreamDescriptorEntry* descriptor =
                    descriptors_.find_stable(binding->descriptor);
                if (descriptor == nullptr) {
                    return Result<rodaq_storage_submission>(activation_status(
                        StatusCode::InvalidState, "Record Descriptor is unavailable"));
                }
                for (std::size_t slot = 0; slot < TimestampState::kMaxSlots; ++slot) {
                    values[slot] = {};
                    const auto reading = record.timestamps().value(slot);
                    if (reading.has_value()) {
                        values[slot].present = RODAQ_TRUE;
                        values[slot].reading = *reading;
                    } else {
                        values[slot].present = RODAQ_FALSE;
                    }
                }
                rodaq_storage_submission submission{};
                submission.struct_size = sizeof(submission);
                submission.operation = operation;
                submission.stream = rodaq_stream_handle{binding->stream.value()};
                submission.descriptor = rodaq_stream_descriptor_handle{binding->descriptor.value()};
                submission.acceptance_sequence = record.acceptance_sequence();
                if (record.source().has_value()) {
                    submission.source_present = RODAQ_TRUE;
                    submission.source.value = record.source()->value();
                }
                if (record.source_sequence().has_value()) {
                    submission.source_sequence_present = RODAQ_TRUE;
                    submission.source_sequence = *record.source_sequence();
                }
                submission.timestamp_values = values.data();
                submission.timestamp_value_count =
                    static_cast<std::uint32_t>(descriptor->canonical_timestamp_keys.size());
                const PayloadView payload = record.record_payload().view();
                submission.payload = rodaq_bytes_view{payload.data(), payload.size()};
                return Result<rodaq_storage_submission>(submission);
            });
        payload_pool_.set_capacity_changed_sink([this] { pipeline_->notify_admission_waiters(); });
        auto started = pipeline_->start();
        if (!started.ok()) {
            pipeline_.reset();
            return started;
        }
        return {};
    } catch (const std::exception& error) {
        return activation_status(StatusCode::InternalError, error.what());
    }
}

bool ActivationState::mark_running() noexcept {
    std::lock_guard<std::mutex> control_lock(control_mutex_);
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    if (state_.load() != DAQState::Starting) {
        return false;
    }
    if (pipeline_ != nullptr) {
        pipeline_->open_admission();
    }
    gate_open_ = pipeline_ != nullptr;
    state_.store(DAQState::Running);
    admission_condition_.notify_all();
    return true;
}

void ActivationState::mark_stopping() noexcept {
    if (!stop_started_valid_) {
        stop_started_ = Clock::now();
        stop_started_valid_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (state_.load() != DAQState::Failed) {
            state_.store(DAQState::Stopping);
        }
    }
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        if (state_.load() != DAQState::Failed) {
            state_.store(DAQState::Stopping);
        }
        gate_open_ = false;
    }
    if (pipeline_ != nullptr) {
        pipeline_->close_admission();
    }
    admission_condition_.notify_all();
}

void ActivationState::mark_stopped() noexcept {
    record_stop_latency();
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        if (state_.load() != DAQState::Failed) {
            state_.store(DAQState::Stopped);
        }
        gate_open_ = false;
    }
    admission_condition_.notify_all();
}

void ActivationState::mark_failed() noexcept {
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        state_.store(DAQState::Failed);
    }
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        state_.store(DAQState::Failed);
        gate_open_ = false;
    }
    if (pipeline_ != nullptr) {
        pipeline_->begin_failure_cutoff();
    }
    admission_condition_.notify_all();
}

bool ActivationState::gate_open() const noexcept {
    std::lock_guard<std::mutex> lock(admission_mutex_);
    return gate_open_;
}

void ActivationState::request_failure(Status error) noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(failure_mutex_);
            if (primary_failure_.ok()) {
                primary_failure_ = std::move(error);
            } else if (secondary_failures_.size() < kMaxSecondaryFailures) {
                secondary_failures_.push_back(std::move(error));
            } else {
                ++secondary_overflow_count_;
            }
        }
        mark_failed();
        if (failure_signal_ != nullptr) {
            failure_signal_->notify();
        }
    } catch (...) {
    }
}

void ActivationState::record_secondary_failure(Status error) noexcept {
    try {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        if (secondary_failures_.size() < kMaxSecondaryFailures) {
            secondary_failures_.push_back(std::move(error));
        } else {
            ++secondary_overflow_count_;
        }
    } catch (...) {
    }
}

bool ActivationState::has_primary_failure() const noexcept {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return !primary_failure_.ok();
}

Status ActivationState::primary_failure() const {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return primary_failure_;
}

Result<std::vector<Status>> ActivationState::secondary_failures() const {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    if (!primary_failure_.ok()) {
        return secondary_failures_;
    }
    return activation_status(StatusCode::InvalidState, "DAQ has no primary failure");
}

std::uint64_t ActivationState::secondary_failure_overflow_count() const noexcept {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return secondary_overflow_count_;
}

Result<DAQLifecycleSnapshot> ActivationState::lifecycle_snapshot() const {
    DAQLifecycleSnapshot snapshot;
    const DAQState state = state_.load();
    snapshot.state = state;
    snapshot.normal_acquisition_allowed = state == DAQState::Running && gate_open();
    snapshot.generation = activation_generation_;
    return snapshot;
}

DAQConfig& ActivationState::options() noexcept { return options_; }
const DAQConfig& ActivationState::options() const noexcept { return options_; }
std::uint64_t ActivationState::activation_generation() const noexcept {
    return activation_generation_;
}
StoragePipeline* ActivationState::pipeline() noexcept { return pipeline_.get(); }
std::optional<plugin::StoragePluginInstance>& ActivationState::storage_instance() noexcept {
    return storage_instance_;
}
std::optional<plugin::AcquisitionPluginInstance>& ActivationState::acquisition_instance(
    std::size_t index) noexcept {
    return acquisition_instances_[index];
}
DAQState ActivationState::state() const noexcept { return state_.load(); }
bool ActivationState::requires_quarantine() const noexcept {
    return control_quarantine_required_.load(std::memory_order_acquire) &&
           active_control_calls_.load(std::memory_order_acquire) != 0;
}
void ActivationState::set_failure_signal(std::shared_ptr<FailureMailbox> signal) noexcept {
    failure_signal_ = std::move(signal);
}

}  // namespace rodaq
