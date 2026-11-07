#include "activation/activation_error.hpp"
#include "activation/activation_state.hpp"
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

using Clock = std::chrono::steady_clock;

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
: config_(std::move(config)),
  limits_{
      config_.resources.max_single_payload_bytes,
      config_.resources.dispatch_capacity,
      config_.resources.operation_capacity,
      config_.resources.completion_capacity,
      config_.stop_progress_timeout,
      config_.stop_progress_timeout,
      config_.stop_progress_timeout,
      config_.stop_progress_timeout,
      config_.stop_progress_timeout,
  },
  activation_generation_(activation_generation),
  arena_([this] {
      auto result = StructuralArena::create(config_.resources.structural_arena_bytes);
      if (!result.ok()) {
          throw std::runtime_error(std::string(result.status().message()));
      }
      return std::move(result.value());
  }()),
  canonical_(config_, activation_generation, arena_->resource()),
  bindings_(config_, activation_generation),
  admission_(config_),
  payload_pool_([this] {
      auto result = PayloadPool::create(config_.resources.payload_pool);
      if (!result.ok()) {
          throw std::runtime_error(std::string(result.status().message()));
      }
      return std::move(result.value());
  }()),
  projections_(ProjectionLimits{128, 8, 1024U * 1024U, 8U * 1024U * 1024U}),
  phase_(ActivationPhase::Starting) {
    secondary_failures_.reserve(kMaxSecondaryFailures);
    descriptor_worker_requests_.resize(config_.resources.max_stream_descriptors);
    acquisition_controls_.resize(config_.acquisitions.size());
    acquisition_instances_.resize(config_.acquisitions.size());
    acquisition_health_.reserve(config_.acquisitions.size());
    for (std::size_t index = 0; index < config_.acquisitions.size(); ++index) {
        acquisition_health_.push_back(
            std::make_unique<std::atomic<std::uint32_t>>(RODAQ_ACQUISITION_PLUGIN_AVAILABLE));
    }
    acquisition_contexts_.reserve(config_.acquisitions.size());
    for (std::size_t index = 0; index < config_.acquisitions.size(); ++index) {
        auto context = std::make_unique<plugin::AcquisitionServiceContext>();
        context->state = this;
        context->plugin_index = index;
        acquisition_contexts_.push_back(std::move(context));
    }
    storage_context_ = std::make_unique<plugin::StorageServiceContext>();
    storage_context_->state = this;
    auto logging_backend = SpdlogBackend::create(config_.logging);
    if (!logging_backend.ok()) {
        throw std::runtime_error(std::string(logging_backend.status().message()));
    }
    auto logging = LoggingContext::create(config_.logging, std::move(logging_backend.value()));
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
    const PayloadPoolConfig& pool_config = config_.resources.payload_pool;
    if (effective_payload_limit == 0 || pool_config.classes.empty() ||
        pool_config.classes.back().max_payload_bytes < effective_payload_limit) {
        return activation_status(
            StatusCode::InvalidArgument,
            "Configured payload block size is below the effective payload limit");
    }
    try {
        pipeline_ = std::make_unique<StoragePipeline>(
            limits_.operation_capacity, limits_.completion_capacity, effective_payload_limit,
            maximum_payload_inflight_bytes, storage, progress_timeout,
            config_.resources.payload_release_workers,
            [this](Status error) { request_failure(std::move(error)); },
            [this](std::size_t bytes) noexcept {
                if (!metrics_.record_payload_released(bytes)) {
                    request_failure(activation_status(
                        StatusCode::InternalError, "DAQ inflight byte accounting underflowed"));
                }
            },
            [this](rodaq_storage_persistence_state persistence) noexcept {
                metrics_.record_persistence(persistence);
                if (!metrics_.record_operation_completed()) {
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
                const BindingEntry* binding = bindings_.bindings().find_stable(record.binding());
                if (binding == nullptr) {
                    return Result<rodaq_storage_submission>(activation_status(
                        StatusCode::InvalidState, "Record Binding is unavailable"));
                }
                const StreamDescriptorEntry* descriptor =
                    canonical_.descriptors().find_stable(binding->descriptor);
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
    if (phase_.load() != ActivationPhase::Starting) {
        return false;
    }
    if (pipeline_ != nullptr) {
        pipeline_->open_admission();
    }
    if (pipeline_ != nullptr) {
        admission_.open_gate();
    } else {
        admission_.close_gate();
    }
    phase_.store(ActivationPhase::Running);
    admission_.notify_all();
    return true;
}

void ActivationState::mark_stopping() noexcept {
    metrics_.mark_stop_started(Clock::now());
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (phase_.load() != ActivationPhase::Failed) {
            phase_.store(ActivationPhase::Stopping);
        }
    }
    admission_.close_gate();
    if (pipeline_ != nullptr) {
        pipeline_->close_admission();
    }
    admission_.notify_all();
}

void ActivationState::mark_stopped() noexcept {
    metrics_.record_stop_latency();
    {
        if (phase_.load() != ActivationPhase::Failed) {
            phase_.store(ActivationPhase::Stopped);
        }
    }
    admission_.close_gate();
    admission_.notify_all();
}

void ActivationState::mark_failed() noexcept {
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        phase_.store(ActivationPhase::Failed);
    }
    phase_.store(ActivationPhase::Failed);
    admission_.close_gate();
    if (pipeline_ != nullptr) {
        pipeline_->begin_failure_cutoff();
    }
    admission_.notify_all();
}

bool ActivationState::gate_open() const noexcept { return admission_.gate_open(); }

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
    const ActivationPhase phase = phase_.load();
    switch (phase) {
        case ActivationPhase::Starting:
            snapshot.state = DAQState::Starting;
            break;
        case ActivationPhase::Running:
            snapshot.state = DAQState::Running;
            break;
        case ActivationPhase::Stopping:
            snapshot.state = DAQState::Stopping;
            break;
        case ActivationPhase::Stopped:
            snapshot.state = DAQState::Stopped;
            break;
        case ActivationPhase::Failed:
            snapshot.state = DAQState::Failed;
            break;
    }
    snapshot.normal_acquisition_allowed = phase == ActivationPhase::Running && gate_open();
    snapshot.generation = activation_generation_;
    return snapshot;
}

const DAQConfig& ActivationState::config() const noexcept { return config_; }
const ActivationLimits& ActivationState::limits() const noexcept { return limits_; }
void ActivationState::apply_limits(const ActivationLimits& limits) noexcept { limits_ = limits; }
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
ActivationPhase ActivationState::phase() const noexcept { return phase_.load(); }
bool ActivationState::requires_quarantine() const noexcept {
    return control_quarantine_required_.load(std::memory_order_acquire) &&
           active_control_calls_.load(std::memory_order_acquire) != 0;
}
void ActivationState::set_failure_signal(std::shared_ptr<FailureMailbox> signal) noexcept {
    failure_signal_ = std::move(signal);
}

Result<PayloadStats> ActivationState::payload_pool_statistics() const {
    try {
        return payload_pool_.stats();
    } catch (const std::bad_alloc&) {
        return activation_status(
            StatusCode::ResourceExhausted, "Payload statistics allocation failed");
    } catch (const std::exception& error) {
        return activation_status(StatusCode::InternalError, error.what());
    }
}

void ActivationState::record_storage_latency(
    StorageLatencyKind kind, std::int64_t duration_ns) noexcept {
    metrics_.record_storage_latency(kind, duration_ns);
}

void ActivationState::record_stop_latency() noexcept { metrics_.record_stop_latency(); }

DAQStats ActivationState::stats() const noexcept {
    return metrics_.snapshot(current_logging_stats());
}

void ActivationState::log(
    LogLevel level, std::string_view category, std::string_view message) noexcept {
    if (logging_context_ != nullptr) {
        logging_context_->log(level, category, {}, message);
    }
}

void ActivationState::diagnostic(
    rodaq_diagnostic_severity severity, std::string_view code, std::string_view message) noexcept {
    LogLevel level = LogLevel::Off;
    switch (severity) {
        case RODAQ_DIAGNOSTIC_INFO:
            level = LogLevel::Info;
            break;
        case RODAQ_DIAGNOSTIC_WARNING:
            level = LogLevel::Warn;
            break;
        case RODAQ_DIAGNOSTIC_ERROR:
            level = LogLevel::Error;
            break;
        case RODAQ_DIAGNOSTIC_FATAL:
            level = LogLevel::Critical;
            break;
        default:
            return;
    }
    try {
        const std::string text = std::string(code) + ": " + std::string(message);
        log(level, "rodaq.diagnostic", text);
    } catch (...) {
    }
}

bool ActivationState::shutdown_logging(std::chrono::steady_clock::time_point deadline) noexcept {
    return logging_context_ == nullptr || logging_context_->shutdown(deadline);
}

void ActivationState::quarantine_logging(std::string reason) noexcept {
    if (logging_context_ != nullptr) {
        logging_context_->quarantine_worker(std::move(reason));
    }
}

LoggingStats ActivationState::current_logging_stats() const noexcept {
    return logging_context_ == nullptr ? LoggingStats{} : logging_context_->stats();
}

Result<Logger> ActivationState::make_logger(std::string_view category) const {
    if (logging_context_ == nullptr) {
        return activation_status(StatusCode::InvalidState, "DAQ logging context is unavailable");
    }
    return logging_context_->make_logger(category);
}

}  // namespace rodaq
