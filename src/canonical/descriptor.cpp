#include "activation/activation_error.hpp"
#include "activation/activation_state.hpp"
#include "canonical/model_ops.hpp"
#include "clock/ingress_time.hpp"
#include "plugin/abi_conversion.hpp"
#include "plugin/acquisition_plugin_instance.hpp"
#include "plugin/plugin_quarantine.hpp"
#include "plugin/storage_plugin_instance.hpp"
#include <rodaq/plugin/services.hpp>

namespace rodaq {

using Clock = std::chrono::steady_clock;

Result<ActivationState::DescriptorReadiness> ActivationState::ensure_descriptor_ready(
    StreamDescriptorHandle descriptor) {
    if (!storage_control_.has_value()) {
        return activation_status(
            StatusCode::InvalidState, "Storage control interface is unavailable");
    }
    const rodaq_interface control = storage_control_.value();
    const auto* table = static_cast<const rodaq_storage_control*>(control.vtable);
    if (table->ensure_descriptor_ready == nullptr) {
        return activation_status(
            StatusCode::InvalidArgument, "Storage descriptor readiness slot is missing");
    }

    std::shared_ptr<ActivationState> state;
    try {
        state = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return activation_status(StatusCode::InvalidState, "DAQ activation has no shared owner");
    }
    auto result = std::make_shared<std::optional<DescriptorReadiness>>();
    auto result_error = std::make_shared<Status>();
    const auto descriptor_value = rodaq_stream_descriptor_handle{descriptor.value()};
    auto guard = track_control_call();
    auto outcome = plugin::PluginQuarantine::process().run(
        [control, table, descriptor_value, result, result_error, state, guard]() -> Result<void> {
            static_cast<void>(state);
            rodaq_descriptor_readiness_result readiness{};
            readiness.struct_size = sizeof(readiness);
            rodaq_status error{};
            const rodaq_status_code status = table->ensure_descriptor_ready(
                control.context, descriptor_value, &readiness, &error);
            if (status != RODAQ_STATUS_OK) {
                auto converted = status_from_abi(status, &error, "storage");
                *result_error = converted.ok() ? activation_status(
                                                     StatusCode::InternalError,
                                                     "Storage descriptor readiness failed")
                                               : std::move(converted);
                return {};
            }
            if (readiness.struct_size < RODAQ_DESCRIPTOR_READINESS_RESULT_MIN_STRUCT_SIZE) {
                *result_error = activation_status(
                    StatusCode::InvalidArgument,
                    "Storage descriptor readiness output is incomplete");
                return {};
            }
            if (readiness.state == RODAQ_DESCRIPTOR_UNSUPPORTED) {
                result->emplace(DescriptorReadiness::Unsupported);
            } else if (readiness.state == RODAQ_DESCRIPTOR_READY) {
                result->emplace(DescriptorReadiness::Ready);
            } else {
                *result_error = readiness.state == RODAQ_DESCRIPTOR_FAILED
                                    ? status_from_abi_diagnostic(readiness.failure)
                                    : activation_status(
                                          StatusCode::InvalidArgument,
                                          "Storage returned invalid descriptor readiness");
            }
            return {};
        },
        Clock::now() + limits_.descriptor_progress_timeout, "Descriptor readiness");
    if (!outcome.ok()) {
        return std::move(outcome).status();
    }
    switch (outcome.value().status) {
        case plugin::SupervisedCallStatus::TimedOut:
            control_quarantine_required_.store(true, std::memory_order_release);
            return activation_status(StatusCode::Timeout, "Storage descriptor readiness timed out");
        case plugin::SupervisedCallStatus::Failed:
            return std::move(outcome.value().error);
        case plugin::SupervisedCallStatus::Completed:
            break;
    }
    if (!result_error->ok()) {
        return std::move(*result_error);
    }
    if (!result->has_value()) {
        return activation_status(
            StatusCode::InternalError, "Descriptor readiness produced no outcome");
    }
    return result->value();
}

Result<void> ActivationState::start_descriptor_reconciliation() {
    std::shared_ptr<ActivationState> state;
    try {
        state = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return activation_status(StatusCode::InvalidState, "DAQ activation has no shared owner");
    }

    std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
    if (descriptor_worker_started_ || descriptor_worker_.joinable()) {
        return activation_status(
            StatusCode::InvalidState, "Descriptor reconciliation is already started");
    }
    descriptor_worker_head_ = 0;
    descriptor_worker_count_ = 0;
    descriptor_worker_busy_ = false;
    descriptor_worker_closing_ = false;
    descriptor_worker_failure_ = {};
    descriptor_worker_started_ = true;
    try {
        descriptor_worker_ = std::thread([state]() { state->run_descriptor_reconciliation(); });
    } catch (const std::exception& error) {
        descriptor_worker_started_ = false;
        return activation_status(
            StatusCode::ResourceExhausted,
            std::string("Failed to start Descriptor reconciliation worker: ") + error.what());
    }
    return {};
}

Result<void> ActivationState::request_descriptor_reconciliation(
    StreamDescriptorHandle descriptor, bool critical_for_start) {
    std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
    if (!descriptor_worker_started_ || descriptor_worker_closing_) {
        return activation_status(
            StatusCode::InvalidState, "Descriptor reconciliation is not accepting work");
    }
    for (std::size_t offset = 0; offset < descriptor_worker_count_; ++offset) {
        DescriptorWorkerRequest& request = descriptor_worker_requests_
            [(descriptor_worker_head_ + offset) % descriptor_worker_requests_.size()];
        if (request.descriptor == descriptor) {
            request.critical_for_start = request.critical_for_start || critical_for_start;
            return {};
        }
    }
    if (descriptor_worker_count_ == descriptor_worker_requests_.size()) {
        return activation_status(
            StatusCode::ResourceExhausted,
            "Descriptor reconciliation request capacity is exhausted");
    }
    const std::size_t tail =
        (descriptor_worker_head_ + descriptor_worker_count_) % descriptor_worker_requests_.size();
    descriptor_worker_requests_[tail] = DescriptorWorkerRequest{descriptor, critical_for_start};
    ++descriptor_worker_count_;
    descriptor_worker_condition_.notify_one();
    return {};
}

Result<void> ActivationState::wait_for_descriptor_reconciliation(
    std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(descriptor_worker_mutex_);
    if (!descriptor_worker_started_ && !descriptor_worker_.joinable()) {
        return activation_status(
            StatusCode::InvalidState, "Descriptor reconciliation is not running");
    }
    if (!descriptor_worker_condition_.wait_until(lock, deadline, [this] {
            return descriptor_worker_count_ == 0 && !descriptor_worker_busy_;
        })) {
        return activation_status(
            StatusCode::Timeout, "Descriptor reconciliation did not become idle");
    }
    if (!descriptor_worker_failure_.ok()) {
        return descriptor_worker_failure_;
    }
    return {};
}

bool ActivationState::close_and_join_descriptor_reconciliation(
    std::chrono::steady_clock::time_point deadline) {
    {
        std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
        descriptor_worker_closing_ = true;
        descriptor_worker_count_ = 0;
        descriptor_worker_condition_.notify_all();
    }
    {
        std::unique_lock<std::mutex> lock(descriptor_worker_mutex_);
        if (!descriptor_worker_condition_.wait_until(
                lock, deadline, [this] { return !descriptor_worker_started_; })) {
            request_failure(activation_status(
                StatusCode::Timeout, "Descriptor reconciliation shutdown timed out"));
            return false;
        }
    }
    if (descriptor_worker_.joinable()) {
        descriptor_worker_.join();
    }
    return true;
}

void ActivationState::run_descriptor_reconciliation() noexcept {
    for (;;) {
        DescriptorWorkerRequest request;
        {
            std::unique_lock<std::mutex> lock(descriptor_worker_mutex_);
            descriptor_worker_condition_.wait(lock, [this] {
                return descriptor_worker_closing_ || descriptor_worker_count_ != 0;
            });
            if (descriptor_worker_closing_) {
                break;
            }
            request = descriptor_worker_requests_[descriptor_worker_head_];
            descriptor_worker_head_ =
                (descriptor_worker_head_ + 1U) % descriptor_worker_requests_.size();
            --descriptor_worker_count_;
            descriptor_worker_busy_ = true;
        }

        Status failure;
        try {
            auto reconciled = reconcile_descriptor(request.descriptor);
            if (!reconciled.ok()) {
                failure = reconciled.status();
            }
        } catch (const std::exception& error) {
            failure = activation_status(StatusCode::InternalError, error.what());
        } catch (...) {
            failure =
                activation_status(StatusCode::InternalError, "Descriptor reconciliation failed");
        }

        bool fatal_failure = false;
        {
            std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
            if (!failure.ok() && (request.critical_for_start || requires_quarantine() ||
                                  phase_.load() == ActivationPhase::Failed)) {
                descriptor_worker_failure_ = failure;
                descriptor_worker_closing_ = true;
                descriptor_worker_count_ = 0;
                fatal_failure = true;
                if (!has_primary_failure()) {
                    request_failure(failure);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
            descriptor_worker_busy_ = false;
            descriptor_worker_condition_.notify_all();
        }
        if (fatal_failure) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
        descriptor_worker_started_ = false;
        descriptor_worker_condition_.notify_all();
    }
}

Result<void> ActivationState::publish_stream_descriptor(
    std::size_t plugin_index, const rodaq_stream_descriptor_registration& request,
    StreamDescriptorHandle& handle) {
    if (!rodaq::plugin::is_valid_stream_descriptor_registration(request) ||
        plugin_index >= config_.acquisitions.size()) {
        return activation_status(
            StatusCode::InvalidArgument, "StreamDescriptor publication is invalid");
    }
    std::string_view name;
    std::string_view payload_encoding;
    parse_abi_string(request.name, &name);
    parse_abi_string(request.payload_encoding, &payload_encoding);

    bool critical_for_start = false;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        const ActivationPhase current_state = phase_.load();
        if (current_state != ActivationPhase::Starting &&
            current_state != ActivationPhase::Running) {
            return activation_status(
                StatusCode::InvalidState, "Descriptor publication is not allowed");
        }
        if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
            RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
            return activation_status(
                StatusCode::InvalidState, "Descriptor publication requires an available Plugin");
        }
        critical_for_start = current_state == ActivationPhase::Starting;
        const StreamHandle stream_handle = StreamHandle::from_raw(request.stream.value);
        StreamEntry* stream = canonical_.streams().find(stream_handle);
        if (stream == nullptr) {
            return activation_status(StatusCode::NotFound, "Descriptor Stream is unknown");
        }
        if (stream->plugin_index != plugin_index) {
            return activation_status(
                StatusCode::InvalidState, "Descriptor Stream belongs to another Plugin");
        }

        std::pmr::vector<TimestampKeyRecord> keys(arena_->resource());
        std::pmr::vector<rodaq_timestamp_key> abi_keys(arena_->resource());
        std::pmr::vector<rodaq_timestamp_key> transport_abi_keys(arena_->resource());
        const std::size_t key_count = static_cast<std::size_t>(request.timestamp_key_count);
        keys.reserve(key_count + 1U);
        abi_keys.reserve(key_count + 1U);
        transport_abi_keys.reserve(request.timestamp_key_count);
        keys.push_back(TimestampKeyRecord{
            copy_string(arena_->resource(), kIngressTimestampRole), ingress_clock_});
        for (std::uint32_t index = 0; index < request.timestamp_key_count; ++index) {
            const rodaq_timestamp_key& key = request.timestamp_keys[index];
            std::string_view role;
            if (!parse_abi_string(key.role, &role)) {
                return activation_status(StatusCode::InvalidArgument, "Timestamp role is invalid");
            }
            if (role == kIngressTimestampRole) {
                return activation_status(
                    StatusCode::InvalidArgument,
                    "Acquisition Plugins must not publish the DAQ ingress timestamp role");
            }
            const ClockHandle clock = ClockHandle::from_raw(key.clock.value);
            const ClockEntry* clock_entry = canonical_.clocks().find(clock);
            if (clock_entry == nullptr) {
                return activation_status(StatusCode::NotFound, "Timestamp Clock is unknown");
            }
            if (clock_entry->plugin_index != plugin_index) {
                return activation_status(
                    StatusCode::InvalidState, "Timestamp Clock belongs to another owner");
            }
            for (std::size_t existing = 1; existing < keys.size(); ++existing) {
                if (keys[existing].clock == clock && strings_equal(keys[existing].role, role)) {
                    return activation_status(
                        StatusCode::InvalidArgument, "TimestampKeySet contains a duplicate key");
                }
            }
            keys.push_back(TimestampKeyRecord{copy_string(arena_->resource(), role), clock});
            transport_abi_keys.push_back(rodaq_timestamp_key{
                rodaq_string_view{keys.back().role.data(), keys.back().role.size()},
                rodaq_clock_handle{clock.value()},
            });
        }

        TimestampKeySetHandle key_set_handle;
        bool found_key_set = false;
        for (TimestampKeySetHandle existing : canonical_.timestamp_key_set_handles()) {
            const TimestampKeySetEntry* entry = canonical_.timestamp_key_sets().find(existing);
            if (entry == nullptr || entry->keys.size() != keys.size()) {
                continue;
            }
            bool equal = true;
            for (std::size_t index = 0; index < keys.size(); ++index) {
                if (!strings_equal(entry->keys[index].role, keys[index].role) ||
                    entry->keys[index].clock != keys[index].clock) {
                    equal = false;
                    break;
                }
            }
            if (equal) {
                key_set_handle = existing;
                found_key_set = true;
                break;
            }
        }

        std::uint64_t key_set_id = 0;
        if (!found_key_set) {
            key_set_id = canonical_.allocate_timestamp_key_set_id();
            TimestampKeySetEntry entry{
                key_set_id,
                std::move(keys),
                std::move(abi_keys),
            };
            auto inserted = canonical_.timestamp_key_sets().insert(std::move(entry));
            if (!inserted.ok()) {
                return std::move(inserted).status();
            }
            key_set_handle = inserted.value();
            canonical_.timestamp_key_set_handles().push_back(key_set_handle);
        }
        TimestampKeySetEntry* key_set = canonical_.timestamp_key_sets().find(key_set_handle);
        if (key_set == nullptr) {
            return activation_status(
                StatusCode::InternalError, "TimestampKeySet publication failed");
        }
        key_set_id = key_set->timestamp_key_set_id;
        if (key_set->abi_keys.size() != key_set->keys.size()) {
            key_set->abi_keys.clear();
            for (const TimestampKeyRecord& key : key_set->keys) {
                key_set->abi_keys.push_back(rodaq_timestamp_key{
                    rodaq_string_view{key.role.data(), key.role.size()},
                    rodaq_clock_handle{key.clock.value()},
                });
            }
        }

        StreamDescriptorEntry descriptor_entry{
            canonical_.allocate_descriptor_id(),
            stream_handle,
            request.schema_present == RODAQ_TRUE,
            SchemaHandle::from_raw(request.schema.value),
            0,
            copy_string(arena_->resource(), name),
            copy_string(arena_->resource(), payload_encoding),
            key_set_id,
            std::pmr::vector<rodaq_timestamp_key>(arena_->resource()),
            std::move(transport_abi_keys),
            plugin_index,
        };
        if (descriptor_entry.schema_present) {
            const SchemaEntry* schema = canonical_.schemas().find(descriptor_entry.schema);
            if (schema == nullptr) {
                return activation_status(StatusCode::NotFound, "Descriptor Schema is unknown");
            }
            descriptor_entry.schema_id = schema->schema_id;
        }
        descriptor_entry.canonical_timestamp_keys.assign(
            key_set->abi_keys.begin(), key_set->abi_keys.end());

        auto descriptor_inserted = canonical_.descriptors().insert(std::move(descriptor_entry));
        if (!descriptor_inserted.ok()) {
            return std::move(descriptor_inserted).status();
        }
        handle = descriptor_inserted.value();
        canonical_.descriptor_handles().push_back(handle);
    }

    auto requested = request_descriptor_reconciliation(handle, critical_for_start);
    if (!requested.ok()) {
        if (requested.status().status_code() == StatusCode::ResourceExhausted) {
            request_failure(requested.status());
        }
        return requested;
    }
    return {};
}

Result<bool> ActivationState::reconcile_descriptor(StreamDescriptorHandle descriptor) {
    while (true) {
        StreamHandle stream;
        std::size_t plugin_index = 0;
        rodaq_interface control{};
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamDescriptorEntry* entry = canonical_.descriptors().find(descriptor);
            if (entry == nullptr) {
                return activation_status(StatusCode::NotFound, "Descriptor is not published");
            }
            if (entry->lifecycle == DescriptorLifecycle::Active) {
                return true;
            }
            if (entry->lifecycle == DescriptorLifecycle::Failed ||
                entry->lifecycle == DescriptorLifecycle::Superseded) {
                return false;
            }
            const ActivationPhase current_state = phase_.load();
            if (current_state != ActivationPhase::Starting &&
                current_state != ActivationPhase::Running) {
                entry->reconciliation_running = false;
                return false;
            }
            if (entry->reconciliation_running) {
                entry->reconciliation_retry_requested = true;
                return false;
            }
            entry->reconciliation_running = true;
            entry->reconciliation_retry_requested = false;
            entry->lifecycle = DescriptorLifecycle::ReadinessPending;
            stream = entry->stream;
            plugin_index = entry->plugin_index;
            if (plugin_index >= acquisition_controls_.size() ||
                !acquisition_controls_[plugin_index].has_value()) {
                entry->reconciliation_running = false;
                entry->lifecycle = DescriptorLifecycle::Failed;
                return activation_status(
                    StatusCode::InvalidState, "Acquisition control interface is unavailable");
            }
            control = acquisition_controls_[plugin_index].value();
        }

        auto readiness = ensure_descriptor_ready(descriptor);
        bool retry = false;
        if (!readiness.ok()) {
            {
                std::lock_guard<std::mutex> lock(control_mutex_);
                StreamDescriptorEntry* entry = canonical_.descriptors().find(descriptor);
                if (entry != nullptr) {
                    entry->reconciliation_running = false;
                    entry->lifecycle = DescriptorLifecycle::Failed;
                    retry = entry->reconciliation_retry_requested;
                    entry->reconciliation_retry_requested = false;
                }
            }
            request_failure(readiness.status());
            return std::move(readiness).status();
        }
        if (readiness.value() != DescriptorReadiness::Ready) {
            {
                std::lock_guard<std::mutex> lock(control_mutex_);
                StreamDescriptorEntry* entry = canonical_.descriptors().find(descriptor);
                if (entry != nullptr) {
                    entry->reconciliation_running = false;
                    entry->lifecycle = DescriptorLifecycle::Unsupported;
                    retry = entry->reconciliation_retry_requested;
                    entry->reconciliation_retry_requested = false;
                    StreamEntry* stream_entry = canonical_.streams().find(entry->stream);
                    if (stream_entry != nullptr &&
                        !bindings_.current_binding(entry->stream).valid()) {
                        stream_entry->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
                    }
                }
            }
            if (retry) {
                continue;
            }
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamDescriptorEntry* entry = canonical_.descriptors().find(descriptor);
            const ActivationPhase current_state = phase_.load();
            if (entry == nullptr || (current_state != ActivationPhase::Starting &&
                                     current_state != ActivationPhase::Running)) {
                if (entry != nullptr) {
                    entry->reconciliation_running = false;
                }
                return false;
            }
            entry->lifecycle = DescriptorLifecycle::Ready;
        }
        auto activated = activate_descriptor_binding(descriptor, stream, plugin_index, control);
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamDescriptorEntry* entry = canonical_.descriptors().find(descriptor);
            if (entry != nullptr) {
                entry->reconciliation_running = false;
                entry->reconciliation_retry_requested = false;
            }
        }
        if (!activated.ok()) {
            return (activated).status();
        }
        return true;
    }
}

void ActivationState::on_projection_registered(
    rodaq_stream_descriptor_handle descriptor_value, std::string_view) noexcept {
    try {
        const StreamDescriptorHandle descriptor =
            StreamDescriptorHandle::from_raw(descriptor_value.value);
        bool critical_for_start = false;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamDescriptorEntry* entry = canonical_.descriptors().find(descriptor);
            if (entry == nullptr) {
                return;
            }
            if (entry->reconciliation_running) {
                entry->reconciliation_retry_requested = true;
                return;
            }
            if (entry->lifecycle == DescriptorLifecycle::Active) {
                return;
            }
            const ActivationPhase current_state = phase_.load();
            if (current_state != ActivationPhase::Starting &&
                current_state != ActivationPhase::Running) {
                return;
            }
            critical_for_start = current_state == ActivationPhase::Starting;
        }
        auto result = request_descriptor_reconciliation(descriptor, critical_for_start);
        if (!result.ok()) {
            if (result.status().status_code() == StatusCode::ResourceExhausted) {
                request_failure(result.status());
            }
        }
    } catch (const std::exception& error) {
        request_failure(activation_status(StatusCode::InternalError, error.what()));
    } catch (...) {
        request_failure(
            activation_status(StatusCode::InternalError, "Descriptor reconciliation failed"));
    }
}

}  // namespace rodaq
