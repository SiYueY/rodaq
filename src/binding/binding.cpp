#include "activation/activation_error.hpp"
#include "activation/activation_state.hpp"
#include "plugin/plugin_quarantine.hpp"
#include <rodaq/plugin/abi_support.hpp>

namespace rodaq {

using Clock = std::chrono::steady_clock;

Result<void> ActivationState::supervise_binding_control(
    std::function<Result<void>()> operation, const char* reason) {
    auto guard = track_control_call();
    auto outcome = plugin::PluginQuarantine::process().run(
        [guard, operation = std::move(operation)]() mutable { return operation(); },
        Clock::now() + limits_.binding_progress_timeout, reason);
    if (!outcome.ok()) {
        return std::move(outcome).status();
    }
    switch (outcome.value().status) {
        case plugin::SupervisedCallStatus::Completed:
            return {};
        case plugin::SupervisedCallStatus::Failed:
            return std::move(outcome.value().error);
        case plugin::SupervisedCallStatus::TimedOut:
            control_quarantine_required_.store(true, std::memory_order_release);
            return activation_status(StatusCode::Timeout, "Acquisition Binding control timed out");
    }
    return activation_status(StatusCode::InternalError, "Unknown Binding control outcome");
}

Result<void> ActivationState::activate_descriptor_binding(
    StreamDescriptorHandle descriptor, StreamHandle stream_handle, std::size_t plugin_index,
    rodaq_interface control) {
    StreamEntry* stream = nullptr;
    StreamBindingState* stream_binding = nullptr;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        stream = canonical_.streams().find(stream_handle);
        if (stream == nullptr) {
            return activation_status(StatusCode::NotFound, "Descriptor Stream is unavailable");
        }
        stream_binding = &bindings_.stream_state(stream_handle);
    }

    std::unique_lock<std::mutex> transition_lock(stream_binding->transition_mutex);
    stream_binding->transition_condition.wait(
        transition_lock, [stream_binding] { return !stream_binding->transition_active; });
    stream_binding->transition_active = true;
    transition_lock.unlock();

    struct TransitionReset {
        StreamBindingState* stream_binding;
        ~TransitionReset() {
            {
                std::lock_guard<std::mutex> lock(stream_binding->transition_mutex);
                stream_binding->transition_active = false;
                stream_binding->transition_token = 0;
            }
            stream_binding->transition_condition.notify_one();
        }
    } transition_reset{stream_binding};

    BindingHandle candidate;
    BindingHandle old_binding;
    std::uint64_t old_binding_value = 0;
    std::uint64_t descriptor_generation = 0;
    rodaq_stream_descriptor_handle old_descriptor{};
    rodaq_binding_transition transition{};
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        StreamDescriptorEntry* descriptor_entry = canonical_.descriptors().find(descriptor);
        StreamEntry* current_stream = canonical_.streams().find(stream_handle);
        const ActivationPhase current_state = phase_.load();
        if (descriptor_entry == nullptr || current_stream == nullptr ||
            descriptor_entry->lifecycle != DescriptorLifecycle::Ready ||
            (current_state != ActivationPhase::Starting &&
             current_state != ActivationPhase::Running)) {
            return activation_status(StatusCode::InvalidState, "Descriptor is no longer ready");
        }
        descriptor_generation = descriptor_entry->stream_descriptor_id;
        old_binding_value = bindings_.current_binding(stream_handle).value();
        if (old_binding_value != 0) {
            BindingEntry* current_binding =
                bindings_.bindings().find(BindingHandle::from_raw(old_binding_value));
            StreamDescriptorEntry* current_descriptor =
                current_binding == nullptr
                    ? nullptr
                    : canonical_.descriptors().find(current_binding->descriptor);
            if (current_descriptor != nullptr &&
                current_descriptor->stream_descriptor_id > descriptor_entry->stream_descriptor_id) {
                descriptor_entry->lifecycle = DescriptorLifecycle::Superseded;
                descriptor_entry->reconciliation_running = false;
                return {};
            }
            old_binding = BindingHandle::from_raw(old_binding_value);
            if (current_binding != nullptr) {
                old_descriptor =
                    rodaq_stream_descriptor_handle{current_binding->descriptor.value()};
            }
        }
        auto created = bindings_.create_binding(
            plugin_index, stream_handle, descriptor, BindingLifecycle::Prepared);
        if (!created.ok()) {
            descriptor_entry->lifecycle = DescriptorLifecycle::Failed;
            return std::move(created).status();
        }
        candidate = created.value();
        descriptor_entry->lifecycle = DescriptorLifecycle::BindingPrepared;
        transition = rodaq_binding_transition{bindings_.next_transition_id()};
    }
    {
        std::lock_guard<std::mutex> lock(stream_binding->transition_mutex);
        stream_binding->transition_token = transition.value;
    }

    const auto* table = static_cast<const rodaq_acquisition_control*>(control.vtable);
    auto control_state = shared_from_this();
    auto prepare_status = std::make_shared<rodaq_status_code>(RODAQ_STATUS_INTERNAL_ERROR);
    auto prepare_error = std::make_shared<rodaq_status>();
    auto prepared_control = supervise_binding_control(
        [control_state, table, control, transition, stream_handle, old_descriptor, descriptor,
         prepare_status, prepare_error]() -> Result<void> {
            static_cast<void>(control_state);
            *prepare_status = table->prepare_binding_switch(
                control.context, transition, rodaq_stream_handle{stream_handle.value()},
                old_descriptor, rodaq_stream_descriptor_handle{descriptor.value()},
                prepare_error.get());
            return {};
        },
        "Acquisition Binding prepare");
    if (!prepared_control.ok()) {
        std::lock_guard<std::mutex> lock(control_mutex_);
        BindingEntry* prepared = bindings_.bindings().find(candidate);
        if (prepared != nullptr) {
            prepared->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
        }
        StreamDescriptorEntry* descriptor_entry = canonical_.descriptors().find(descriptor);
        if (descriptor_entry != nullptr) {
            descriptor_entry->lifecycle = DescriptorLifecycle::Failed;
        }
        return prepared_control;
    }
    if (*prepare_status != RODAQ_STATUS_OK) {
        bool resume_old = false;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamEntry* current_stream = canonical_.streams().find(stream_handle);
            const ActivationPhase current_state = phase_.load();
            resume_old = old_binding.valid() && current_stream != nullptr &&
                         current_state == ActivationPhase::Running &&
                         bindings_.current_binding(stream_handle).value() == old_binding_value &&
                         current_stream->availability->load() == RODAQ_AVAILABILITY_AVAILABLE;
        }
        auto abort_error = std::make_shared<rodaq_status>();
        auto abort_status = std::make_shared<rodaq_status_code>(RODAQ_STATUS_INTERNAL_ERROR);
        auto aborted = supervise_binding_control(
            [control_state, table, control, transition, resume_old, abort_status, abort_error]() {
                static_cast<void>(control_state);
                *abort_status = table->abort_binding_switch(
                    control.context, transition,
                    resume_old ? RODAQ_BINDING_ABORT_RESUME_OLD : RODAQ_BINDING_ABORT_KEEP_PAUSED,
                    abort_error.get());
                return Result<void>();
            },
            "Acquisition Binding abort");
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            BindingEntry* prepared = bindings_.bindings().find(candidate);
            if (prepared != nullptr) {
                prepared->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
            }
            StreamDescriptorEntry* descriptor_entry = canonical_.descriptors().find(descriptor);
            if (descriptor_entry != nullptr) {
                descriptor_entry->lifecycle = DescriptorLifecycle::Failed;
            }
            StreamEntry* current_stream = canonical_.streams().find(stream_handle);
            if (!resume_old && current_stream != nullptr) {
                current_stream->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
            }
        }
        if (!aborted.ok()) {
            request_failure(std::move(aborted).status());
        } else if (*abort_status != RODAQ_STATUS_OK) {
            request_failure(activation_status(
                StatusCode::InternalError, "Binding abort after prepare failure also failed"));
        }
        auto converted = status_from_abi(*prepare_status, prepare_error.get(), "acquisition");
        return converted.ok() ? activation_status(StatusCode::Unavailable, "Binding prepare failed")
                              : std::move(converted);
    }

    bool transition_stale = false;
    {
        std::lock_guard<std::mutex> stream_lock(admission_.stream_mutex(stream_handle));
        std::lock_guard<std::mutex> control_lock(control_mutex_);
        bool transition_token_current = false;
        {
            std::lock_guard<std::mutex> transition_guard(stream_binding->transition_mutex);
            transition_token_current = stream_binding->transition_active &&
                                       stream_binding->transition_token == transition.value;
        }
        StreamDescriptorEntry* descriptor_entry = canonical_.descriptors().find(descriptor);
        BindingEntry* prepared = bindings_.bindings().find(candidate);
        StreamEntry* current_stream = canonical_.streams().find(stream_handle);
        BindingEntry* old = old_binding.valid() ? bindings_.bindings().find(old_binding) : nullptr;
        const ActivationPhase current_state = phase_.load();
        transition_stale = descriptor_entry == nullptr || prepared == nullptr ||
                           current_stream == nullptr ||
                           descriptor_entry->stream_descriptor_id != descriptor_generation ||
                           descriptor_entry->lifecycle != DescriptorLifecycle::BindingPrepared ||
                           bindings_.current_binding(stream_handle).value() != old_binding_value ||
                           (old_binding.valid() && old == nullptr) || !transition_token_current ||
                           (current_state != ActivationPhase::Starting &&
                            current_state != ActivationPhase::Running);
        if (transition_stale) {
            if (prepared != nullptr) {
                prepared->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
            }
            if (descriptor_entry != nullptr) {
                descriptor_entry->lifecycle = current_state == ActivationPhase::Stopping ||
                                                      current_state == ActivationPhase::Failed
                                                  ? DescriptorLifecycle::Superseded
                                                  : DescriptorLifecycle::Failed;
            }
        } else {
            prepared->lifecycle->store(
                static_cast<std::uint32_t>(BindingLifecycle::Active), std::memory_order_release);
            if (old != nullptr) {
                old->lifecycle->store(
                    static_cast<std::uint32_t>(BindingLifecycle::Draining),
                    std::memory_order_release);
            }
            bindings_.set_current_binding(stream_handle, candidate);
            current_stream->availability->store(RODAQ_AVAILABILITY_AVAILABLE);
            descriptor_entry->lifecycle = DescriptorLifecycle::Active;
        }
    }
    notify_admission_eligibility_changed();
    if (transition_stale) {
        const rodaq_binding_abort_disposition disposition =
            old_binding.valid() && phase_.load() == ActivationPhase::Running
                ? RODAQ_BINDING_ABORT_RESUME_OLD
                : RODAQ_BINDING_ABORT_KEEP_PAUSED;
        auto abort_status = std::make_shared<rodaq_status_code>(RODAQ_STATUS_INTERNAL_ERROR);
        auto abort_error = std::make_shared<rodaq_status>();
        auto aborted = supervise_binding_control(
            [control_state, table, control, transition, disposition, abort_status, abort_error]() {
                static_cast<void>(control_state);
                *abort_status = table->abort_binding_switch(
                    control.context, transition, disposition, abort_error.get());
                return Result<void>();
            },
            "Acquisition Binding stale-transition abort");
        if (!aborted.ok() || *abort_status != RODAQ_STATUS_OK) {
            request_failure(
                aborted.ok() ? activation_status(
                                   StatusCode::Unavailable, "Binding stale-transition abort failed")
                             : aborted.status());
        }
        return activation_status(StatusCode::InvalidState, "Binding transition became stale");
    }

    auto commit_status = std::make_shared<rodaq_status_code>(RODAQ_STATUS_INTERNAL_ERROR);
    auto commit_error = std::make_shared<rodaq_status>();
    auto committed = supervise_binding_control(
        [control_state, table, control, transition, commit_status, commit_error]() {
            static_cast<void>(control_state);
            *commit_status =
                table->commit_binding_switch(control.context, transition, commit_error.get());
            return Result<void>();
        },
        "Acquisition Binding commit");
    if (!committed.ok() || *commit_status != RODAQ_STATUS_OK) {
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamEntry* current_stream = canonical_.streams().find(stream_handle);
            if (current_stream != nullptr) {
                current_stream->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
            }
        }
        request_failure(
            committed.ok()
                ? activation_status(
                      StatusCode::Unavailable, "Binding commit failed after DAQ publication")
                : committed.status());
        auto converted = committed.ok()
                             ? status_from_abi(*commit_status, commit_error.get(), "acquisition")
                             : committed.status();
        return converted.ok() ? activation_status(StatusCode::Unavailable, "Binding commit failed")
                              : std::move(converted);
    }
    if (old_binding.valid()) {
        std::lock_guard<std::mutex> lock(control_mutex_);
        BindingEntry* old = bindings_.bindings().find(old_binding);
        if (old != nullptr && old->admission_inflight->load() == 0) {
            old->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
        }
    }
    return {};
}

void ActivationState::retire_draining_binding(BindingHandle binding) noexcept {
    std::lock_guard<std::mutex> lock(control_mutex_);
    static_cast<void>(bindings_.retire_if_drained(binding));
}

}  // namespace rodaq
