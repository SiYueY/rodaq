#include "activation/activation_error.hpp"
#include "activation/activation_state.hpp"
#include "plugin/abi_conversion.hpp"
#include "plugin/acquisition_plugin_instance.hpp"

namespace rodaq {

void ActivationState::notify_admission_eligibility_changed() noexcept {
    admission_.notify_eligibility_changed();
    if (pipeline_ != nullptr) {
        pipeline_->notify_admission_waiters();
    }
}

Result<void> ActivationState::report_source_availability(
    std::size_t plugin_index, SourceHandle source, rodaq_availability_state availability) {
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        auto reported = canonical_.report_source_availability(plugin_index, source, availability);
        if (!reported.ok()) {
            return reported;
        }
    }
    notify_admission_eligibility_changed();
    return {};
}

Result<void> ActivationState::report_stream_availability(
    std::size_t plugin_index, StreamHandle stream, rodaq_availability_state availability) {
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        auto reported = canonical_.report_stream_availability(plugin_index, stream, availability);
        if (!reported.ok()) {
            return reported;
        }
    }
    notify_admission_eligibility_changed();
    return {};
}

Result<void> ActivationState::report_plugin_state(
    std::size_t plugin_index, rodaq_acquisition_plugin_state plugin_state,
    const rodaq_status* diagnostic) {
    if (plugin_index >= acquisition_health_.size() ||
        (plugin_state != RODAQ_ACQUISITION_PLUGIN_AVAILABLE &&
         plugin_state != RODAQ_ACQUISITION_PLUGIN_RECOVERABLE_FAILURE &&
         plugin_state != RODAQ_ACQUISITION_PLUGIN_TERMINAL_FAILURE)) {
        return activation_status(StatusCode::InvalidArgument, "Plugin state is invalid");
    }

    bool notify_eligibility = false;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        const std::uint32_t previous =
            acquisition_health_[plugin_index]->load(std::memory_order_acquire);
        if (previous == RODAQ_ACQUISITION_PLUGIN_TERMINAL_FAILURE &&
            plugin_state != RODAQ_ACQUISITION_PLUGIN_TERMINAL_FAILURE) {
            return activation_status(
                StatusCode::InvalidState, "Terminal Plugin failure cannot recover");
        }
        acquisition_health_[plugin_index]->store(plugin_state, std::memory_order_release);

        if (plugin_state != RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
            for (SourceHandle handle : canonical_.source_handles()) {
                SourceEntry* source = canonical_.sources().find(handle);
                if (source != nullptr && source->plugin_index == plugin_index) {
                    source->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
                }
            }
            for (StreamHandle handle : canonical_.stream_handles()) {
                StreamEntry* stream = canonical_.streams().find(handle);
                if (stream == nullptr || stream->plugin_index != plugin_index) {
                    continue;
                }
                stream->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
                bindings_.stream_state(handle).current_binding.store(0, std::memory_order_release);
            }
            for (BindingHandle handle : bindings_.binding_handles()) {
                BindingEntry* binding = bindings_.bindings().find(handle);
                if (binding != nullptr && binding->plugin_index == plugin_index) {
                    binding->lifecycle->store(
                        static_cast<std::uint32_t>(BindingLifecycle::Retired),
                        std::memory_order_release);
                }
            }
            for (StreamDescriptorHandle handle : canonical_.descriptor_handles()) {
                StreamDescriptorEntry* descriptor = canonical_.descriptors().find(handle);
                if (descriptor != nullptr && descriptor->plugin_index == plugin_index &&
                    descriptor->lifecycle != DescriptorLifecycle::Failed) {
                    descriptor->lifecycle = DescriptorLifecycle::Superseded;
                }
            }
            notify_eligibility = true;
        }
    }

    if (notify_eligibility) {
        notify_admission_eligibility_changed();
    }
    if (plugin_state == RODAQ_ACQUISITION_PLUGIN_TERMINAL_FAILURE) {
        request_failure(
            diagnostic == nullptr
                ? activation_status(
                      StatusCode::Unavailable, "Acquisition Plugin reported terminal failure")
                : status_from_abi_diagnostic(*diagnostic));
    }
    return {};
}

}  // namespace rodaq
