#include "activation/activation_internal.hpp"
#include "plugin/acquisition_plugin_instance.hpp"
#include "plugin/storage_plugin_instance.hpp"
#include <rodaq/plugin/services.hpp>

namespace rodaq {

namespace {

struct SubmissionCallGuard {
    explicit SubmissionCallGuard(ActivationState& runtime_state) noexcept : state_(&runtime_state) {
        runtime_state.begin_submission();
    }
    ~SubmissionCallGuard() { state_->end_submission(); }

    ActivationState* state_;
};

rodaq_acquisition_submit_result pre_rejected(
    ActivationState& state, rodaq_admission_rejection_reason reason,
    rodaq_payload_disposition disposition, bool gate_closed = false,
    bool timeout = false) noexcept {
    state.record_pre_rejected(gate_closed, timeout);
    return submit_result(RODAQ_ADMISSION_PRE_REJECTED, reason, disposition);
}

}  // namespace

bool ActivationState::begin_submission() noexcept {
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        ++active_submission_calls_;
    }
    return true;
}

void ActivationState::end_submission() noexcept {
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        --active_submission_calls_;
    }
    admission_condition_.notify_all();
}

bool ActivationState::wait_for_admission_drain(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(admission_mutex_);
    return admission_condition_.wait_until(lock, deadline, [this] {
        return active_submission_calls_ == 0 && admission_inflight_ == 0;
    });
}

bool ActivationState::wait_for_submissions(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(admission_mutex_);
    return admission_condition_.wait_until(
        lock, deadline, [this] { return active_submission_calls_ == 0; });
}

Result<rodaq_acquisition_submit_result> ActivationState::submit(
    std::size_t plugin_index, const rodaq_record_submission& submission,
    rodaq_payload_disposition& disposition) {
    SubmissionCallGuard submission_guard(*this);

    const bool structural_valid = rodaq::plugin::is_valid_record_submission(submission);
    if (!structural_valid) {
        const bool external = submission.payload.mode == RODAQ_PAYLOAD_ADOPT ||
                              submission.payload.mode == RODAQ_PAYLOAD_RETAIN;
        disposition = external ? RODAQ_PAYLOAD_DISPOSITION_CALLER_RESPONSIBLE
                               : RODAQ_PAYLOAD_DISPOSITION_NOT_APPLICABLE;
        return pre_rejected(*this, RODAQ_REJECTION_INVALID_INPUT, disposition);
    }

    const rodaq_payload_mode payload_mode = submission.payload.mode;
    disposition = payload_mode == RODAQ_PAYLOAD_COPY ? RODAQ_PAYLOAD_DISPOSITION_NOT_APPLICABLE
                                                     : RODAQ_PAYLOAD_DISPOSITION_CALLER_RESPONSIBLE;
    if (state_.load() != DAQState::Running || !gate_open() || pipeline_ == nullptr) {
        return pre_rejected(*this, RODAQ_REJECTION_RUNTIME_NOT_RUNNING, disposition, true);
    }
    if (plugin_index >= acquisition_health_.size() ||
        acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
            RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return pre_rejected(*this, RODAQ_REJECTION_RUNTIME_FAILURE, disposition);
    }

    const StreamHandle stream_handle = StreamHandle::from_raw(submission.stream.value);
    const StreamDescriptorHandle descriptor_handle =
        StreamDescriptorHandle::from_raw(submission.descriptor.value);
    std::optional<SourceHandle> source;
    std::optional<std::uint64_t> source_sequence;
    if (submission.source_present == RODAQ_TRUE) {
        source = SourceHandle::from_raw(submission.source.value);
    }
    if (submission.source_sequence_present == RODAQ_TRUE) {
        source_sequence = submission.source_sequence;
    }
    StreamEntry* stream = nullptr;
    const StreamDescriptorEntry* descriptor = nullptr;
    const SourceEntry* source_entry = nullptr;
    BindingHandle current_binding;
    stream = streams_.find_stable(stream_handle);
    descriptor = descriptors_.find_stable(descriptor_handle);
    if (stream == nullptr || descriptor == nullptr) {
        return pre_rejected(*this, RODAQ_REJECTION_STALE_HANDLE, disposition);
    }
    if (stream->plugin_index != plugin_index || descriptor->plugin_index != plugin_index) {
        return pre_rejected(*this, RODAQ_REJECTION_STALE_HANDLE, disposition);
    }
    if (descriptor->transport_timestamp_keys.size() != submission.timestamp_value_count) {
        return pre_rejected(*this, RODAQ_REJECTION_INVALID_INPUT, disposition);
    }
    if (source.has_value()) {
        source_entry = sources_.find_stable(*source);
        if (source_entry == nullptr || source_entry->plugin_index != plugin_index) {
            return pre_rejected(*this, RODAQ_REJECTION_STALE_HANDLE, disposition);
        }
    }
    current_binding =
        BindingHandle::from_raw(stream->current_binding->load(std::memory_order_acquire));
    BindingEntry* binding =
        current_binding.valid() ? bindings_.find_stable(current_binding) : nullptr;
    if (binding == nullptr) {
        return pre_rejected(*this, RODAQ_REJECTION_BINDING_NOT_ACTIVE, disposition);
    }
    if (binding->descriptor != descriptor_handle) {
        return pre_rejected(*this, RODAQ_REJECTION_BINDING_NOT_CURRENT, disposition);
    }
    const auto binding_lifecycle =
        static_cast<BindingLifecycle>(binding->lifecycle->load(std::memory_order_acquire));
    if (binding_lifecycle == BindingLifecycle::Draining ||
        binding_lifecycle == BindingLifecycle::Retired) {
        return pre_rejected(*this, RODAQ_REJECTION_BINDING_NOT_CURRENT, disposition);
    }
    if (binding_lifecycle != BindingLifecycle::Active) {
        return pre_rejected(*this, RODAQ_REJECTION_BINDING_NOT_ACTIVE, disposition);
    }
    if (stream->availability->load() != RODAQ_AVAILABILITY_AVAILABLE) {
        return pre_rejected(*this, RODAQ_REJECTION_STREAM_UNAVAILABLE, disposition);
    }
    if (source_entry != nullptr) {
        if (source_entry->availability->load() != RODAQ_AVAILABILITY_AVAILABLE) {
            return pre_rejected(*this, RODAQ_REJECTION_SOURCE_UNAVAILABLE, disposition);
        }
    }

    const std::size_t payload_size = static_cast<std::size_t>(submission.payload.size);
    if (payload_size > options_.resources.max_single_payload_bytes) {
        return pre_rejected(*this, RODAQ_REJECTION_PAYLOAD_TOO_LARGE, disposition);
    }
    if (submission.payload.data == nullptr && payload_size != 0) {
        return pre_rejected(*this, RODAQ_REJECTION_PAYLOAD_INVALID, disposition);
    }

    TimestampState timestamps;
    auto ingress_reading = capture_ingress_reading();
    if (!ingress_reading.ok()) {
        request_failure(ingress_reading.status());
        return std::move(ingress_reading).status();
    }
    auto ingress_set = timestamps.set(0, ingress_reading.value());
    if (!ingress_set.ok()) {
        request_failure(ingress_set.status());
        return (ingress_set).status();
    }
    for (std::size_t slot = 0; slot < descriptor->transport_timestamp_keys.size(); ++slot) {
        const rodaq_timestamp_value& value = submission.timestamp_values[slot];
        if (value.present == RODAQ_TRUE) {
            const auto set = timestamps.set(slot + 1U, value.reading);
            if (!set.ok()) {
                return pre_rejected(*this, RODAQ_REJECTION_INVALID_INPUT, disposition);
            }
        }
    }

    if (!sequences_.can_allocate(stream_handle)) {
        stream->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
        return pre_rejected(*this, RODAQ_REJECTION_SEQUENCE_EXHAUSTED, disposition);
    }

    const auto deadline = Clock::now() + options_.submit_wait_timeout;
    const auto admission_wait_started = Clock::now();
    auto acquired = pipeline_->acquire_pre_gate(
        payload_size, options_.admission_mode, &payload_pool_, payload_mode == RODAQ_PAYLOAD_COPY,
        deadline, admission_eligibility_generation_);
    record_admission_wait(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - admission_wait_started)
            .count());
    if (!acquired.ok()) {
        return std::move(acquired).status();
    }
    switch (acquired.value().outcome) {
        case PreGateAcquireOutcome::GateClosed:
            return pre_rejected(*this, RODAQ_REJECTION_GATE_CLOSED, disposition, true);
        case PreGateAcquireOutcome::Timeout:
            return pre_rejected(*this, RODAQ_REJECTION_WAIT_TIMEOUT, disposition, false, true);
        case PreGateAcquireOutcome::ResourceUnavailable:
            return pre_rejected(*this, RODAQ_REJECTION_RESOURCE_UNAVAILABLE, disposition);
        case PreGateAcquireOutcome::EligibilityChanged: {
            if (state_.load() != DAQState::Running || !gate_open()) {
                return pre_rejected(*this, RODAQ_REJECTION_GATE_CLOSED, disposition, true);
            }
            const BindingHandle binding_handle =
                BindingHandle::from_raw(stream->current_binding->load(std::memory_order_acquire));
            const BindingEntry* binding_entry =
                binding_handle.valid() ? bindings_.find_stable(binding_handle) : nullptr;
            if (binding_entry == nullptr || binding_entry->descriptor != descriptor_handle) {
                return pre_rejected(*this, RODAQ_REJECTION_BINDING_NOT_CURRENT, disposition);
            }
            if (stream->availability->load(std::memory_order_acquire) !=
                RODAQ_AVAILABILITY_AVAILABLE) {
                return pre_rejected(*this, RODAQ_REJECTION_STREAM_UNAVAILABLE, disposition);
            }
            if (source_entry != nullptr &&
                source_entry->availability->load(std::memory_order_acquire) !=
                    RODAQ_AVAILABILITY_AVAILABLE) {
                return pre_rejected(*this, RODAQ_REJECTION_SOURCE_UNAVAILABLE, disposition);
            }
            return pre_rejected(*this, RODAQ_REJECTION_BINDING_NOT_ACTIVE, disposition);
        }
        case PreGateAcquireOutcome::Acquired:
            break;
    }
    PipelineReservation reservation = std::move(acquired.value().reservation);

    const PayloadView payload_view(submission.payload.data, payload_size);
    Payload payload;
    if (payload_mode == RODAQ_PAYLOAD_COPY) {
        auto copied =
            payload_pool_.materialize(reservation.take_payload_pool_reservation(), payload_view);
        if (!copied.ok()) {
            return pre_rejected(
                *this,
                copied.status().status_code() == StatusCode::ResourceExhausted
                    ? RODAQ_REJECTION_RESOURCE_UNAVAILABLE
                    : RODAQ_REJECTION_PAYLOAD_INVALID,
                disposition);
        }
        payload = std::move(copied.value());
    } else if (payload_mode == RODAQ_PAYLOAD_ADOPT) {
        if (plugin_index >= acquisition_instances_.size() ||
            !acquisition_instances_[plugin_index].has_value()) {
            return pre_rejected(*this, RODAQ_REJECTION_RUNTIME_FAILURE, disposition);
        }
        auto adopted = Payload::adopt(
            payload_view, submission.payload.owner_context, submission.payload.release);
        if (!adopted.ok()) {
            return pre_rejected(*this, RODAQ_REJECTION_PAYLOAD_INVALID, disposition);
        }
        payload = std::move(adopted.value());
        payload.attach_lifetime_guard(
            acquisition_instances_[plugin_index]->lease().lifetime_owner());
        disposition = RODAQ_PAYLOAD_DISPOSITION_RUNTIME_RESPONSIBLE;
    } else {
        if (plugin_index >= acquisition_instances_.size() ||
            !acquisition_instances_[plugin_index].has_value()) {
            return pre_rejected(*this, RODAQ_REJECTION_RUNTIME_FAILURE, disposition);
        }
        auto retained = Payload::retain(
            payload_view, submission.payload.owner_context, submission.payload.release);
        if (!retained.ok()) {
            return pre_rejected(*this, RODAQ_REJECTION_PAYLOAD_INVALID, disposition);
        }
        payload = std::move(retained.value());
        payload.attach_lifetime_guard(
            acquisition_instances_[plugin_index]->lease().lifetime_owner());
        disposition = RODAQ_PAYLOAD_DISPOSITION_RUNTIME_RESPONSIBLE;
    }

    bool admission_failure = false;
    bool retire_binding = false;
    Status deferred_failure;
    std::optional<rodaq_acquisition_submit_result> deferred_result;
    {
        std::lock_guard<std::mutex> stream_lock(*stream->admission_mutex);
        std::lock_guard<std::mutex> gate_lock(admission_mutex_);
        if (state_.load() != DAQState::Running || !gate_open_) {
            return pre_rejected(*this, RODAQ_REJECTION_GATE_CLOSED, disposition, true);
        }
        if (plugin_index >= acquisition_health_.size() ||
            acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
                RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
            return pre_rejected(*this, RODAQ_REJECTION_RUNTIME_FAILURE, disposition);
        }
        if (!pipeline_->admission_open()) {
            admission_failure = true;
        }
        const BindingHandle final_binding =
            BindingHandle::from_raw(stream->current_binding->load(std::memory_order_acquire));
        BindingEntry* final_binding_entry =
            final_binding.valid() ? bindings_.find_stable(final_binding) : nullptr;
        if (final_binding_entry == nullptr ||
            final_binding_entry->descriptor != descriptor_handle) {
            return pre_rejected(*this, RODAQ_REJECTION_BINDING_NOT_CURRENT, disposition);
        }
        if (final_binding_entry->lifecycle->load(std::memory_order_acquire) !=
            static_cast<std::uint32_t>(BindingLifecycle::Active)) {
            return pre_rejected(*this, RODAQ_REJECTION_BINDING_NOT_ACTIVE, disposition);
        }
        if (stream->availability->load() != RODAQ_AVAILABILITY_AVAILABLE) {
            return pre_rejected(*this, RODAQ_REJECTION_STREAM_UNAVAILABLE, disposition);
        }
        if (source.has_value()) {
            if (source_entry == nullptr ||
                source_entry->availability->load() != RODAQ_AVAILABILITY_AVAILABLE) {
                return pre_rejected(*this, RODAQ_REJECTION_SOURCE_UNAVAILABLE, disposition);
            }
        }
        if (!sequences_.can_allocate(stream_handle)) {
            stream->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
            return pre_rejected(*this, RODAQ_REJECTION_SEQUENCE_EXHAUSTED, disposition);
        }
        if (admission_failure) {
            deferred_failure = activation_status(
                StatusCode::InvalidState, "Global Gate and Storage admission state disagree");
            deferred_result.emplace(
                pre_rejected(*this, RODAQ_REJECTION_RUNTIME_FAILURE, disposition));
        } else {
            ++admission_inflight_;
            final_binding_entry->admission_inflight->fetch_add(1);
            auto sequence = sequences_.allocate(stream_handle);
            if (!sequence.ok()) {
                --admission_inflight_;
                final_binding_entry->admission_inflight->fetch_sub(1);
                admission_condition_.notify_all();
                deferred_failure = sequence.status();
                deferred_result.emplace(submit_result(
                    RODAQ_ADMISSION_REJECTED, RODAQ_REJECTION_RUNTIME_FAILURE, disposition));
            } else {
                const std::int64_t accepted_steady_time_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now().time_since_epoch())
                        .count();
                auto record = AcceptedRecord::create(
                    final_binding, sequence.value(), source, source_sequence, timestamps,
                    std::move(payload), reservation.take_record_reservation(),
                    accepted_steady_time_ns);
                if (!record.ok()) {
                    --admission_inflight_;
                    final_binding_entry->admission_inflight->fetch_sub(1);
                    admission_condition_.notify_all();
                    stats_.record_rejected();
                    deferred_failure = record.status();
                    deferred_result.emplace(submit_result(
                        RODAQ_ADMISSION_REJECTED, RODAQ_REJECTION_RUNTIME_FAILURE, disposition));
                } else {
                    stats_.record_inflight_acquired(static_cast<std::uint64_t>(payload_size));
                    pipeline_->publish(std::move(reservation), std::move(record.value()));
                    stats_.record_accepted(payload_size);
                    --admission_inflight_;
                    const std::uint64_t binding_inflight =
                        final_binding_entry->admission_inflight->fetch_sub(1) - 1U;
                    admission_condition_.notify_all();
                    retire_binding = binding_inflight == 0 &&
                                     final_binding_entry->lifecycle->load() ==
                                         static_cast<std::uint32_t>(BindingLifecycle::Draining);
                }
            }
        }
    }
    if (!deferred_failure.ok()) {
        request_failure(std::move(deferred_failure));
    }
    if (retire_binding) {
        retire_draining_binding(current_binding);
    }
    if (deferred_result.has_value()) {
        return deferred_result.value();
    }
    return submit_result(RODAQ_ADMISSION_ACCEPTED, RODAQ_REJECTION_NONE, disposition);
}

void ActivationState::record_pre_rejected(bool gate_closed, bool timeout) noexcept {
    if (gate_closed) {
        stats_.record_gate_closed();
    } else if (timeout) {
        stats_.record_timed_out();
    } else {
        stats_.record_rejected();
    }
}

void ActivationState::record_admission_wait(std::int64_t duration_ns) noexcept {
    if (duration_ns >= 0 && admission_wait_latency_ != nullptr) {
        static_cast<void>(admission_wait_latency_->record(duration_ns));
    }
}

}  // namespace rodaq
