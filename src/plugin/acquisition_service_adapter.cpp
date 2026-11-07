#include "activation/activation_state.hpp"
#include "plugin/abi_conversion.hpp"
#include "plugin/service_context.hpp"

#include <cstdint>
#include <exception>
#include <string_view>
#include <utility>

namespace rodaq {
namespace {

rodaq_status_code assign_status(const Status& error, rodaq_status* out_status) noexcept {
    assign_abi_status(error, out_status);
    return abi_status_from_status_code(error.code());
}

rodaq_status_code assign_message(
    StatusCode status_code, std::string_view message, rodaq_status* out_status) noexcept {
    assign_abi_status(abi_status_from_status_code(status_code), "plugin", 0, message, out_status);
    return abi_status_from_status_code(status_code);
}

const plugin::AcquisitionServiceContext* context_from(void* context) noexcept {
    return static_cast<const plugin::AcquisitionServiceContext*>(context);
}

rodaq_status_code acquisition_register_source(
    void* context, const rodaq_source_registration* request, rodaq_source_handle* out_handle,
    rodaq_status* out_status) noexcept {
    if (context == nullptr || request == nullptr || out_handle == nullptr) {
        return assign_message(
            StatusCode::InvalidArgument, "source registration input is null", out_status);
    }
    *out_handle = {};
    try {
        const auto* service_context = context_from(context);
        SourceHandle handle;
        const auto result = service_context->state->register_source(
            service_context->plugin_index, *request, handle);
        if (!result.ok()) {
            return assign_status(result.status(), out_status);
        }
        out_handle->value = handle.value();
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(StatusCode::InternalError, "source registration failed", out_status);
    }
}

rodaq_status_code acquisition_register_clock(
    void* context, const rodaq_clock_registration* request, rodaq_clock_handle* out_handle,
    rodaq_status* out_status) noexcept {
    if (context == nullptr || request == nullptr || out_handle == nullptr) {
        return assign_message(
            StatusCode::InvalidArgument, "clock registration input is null", out_status);
    }
    *out_handle = {};
    try {
        const auto* service_context = context_from(context);
        ClockHandle handle;
        const auto result =
            service_context->state->register_clock(service_context->plugin_index, *request, handle);
        if (!result.ok()) {
            return assign_status(result.status(), out_status);
        }
        out_handle->value = handle.value();
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(StatusCode::InternalError, "clock registration failed", out_status);
    }
}

rodaq_status_code acquisition_register_schema(
    void* context, const rodaq_schema_registration* request, rodaq_schema_handle* out_handle,
    rodaq_status* out_status) noexcept {
    if (context == nullptr || request == nullptr || out_handle == nullptr) {
        return assign_message(
            StatusCode::InvalidArgument, "schema registration input is null", out_status);
    }
    *out_handle = {};
    try {
        const auto* service_context = context_from(context);
        SchemaHandle handle;
        const auto result = service_context->state->register_schema(
            service_context->plugin_index, *request, handle);
        if (!result.ok()) {
            return assign_status(result.status(), out_status);
        }
        out_handle->value = handle.value();
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(StatusCode::InternalError, "schema registration failed", out_status);
    }
}

rodaq_status_code acquisition_register_stream(
    void* context, const rodaq_stream_registration* request, rodaq_stream_handle* out_handle,
    rodaq_status* out_status) noexcept {
    if (context == nullptr || request == nullptr || out_handle == nullptr) {
        return assign_message(
            StatusCode::InvalidArgument, "stream registration input is null", out_status);
    }
    *out_handle = {};
    try {
        const auto* service_context = context_from(context);
        StreamHandle handle;
        const auto result = service_context->state->register_stream(
            service_context->plugin_index, *request, handle);
        if (!result.ok()) {
            return assign_status(result.status(), out_status);
        }
        out_handle->value = handle.value();
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(StatusCode::InternalError, "stream registration failed", out_status);
    }
}

rodaq_status_code acquisition_publish_descriptor(
    void* context, const rodaq_stream_descriptor_registration* request,
    rodaq_stream_descriptor_handle* out_handle, rodaq_status* out_status) noexcept {
    if (context == nullptr || request == nullptr || out_handle == nullptr) {
        return assign_message(
            StatusCode::InvalidArgument, "descriptor publication input is null", out_status);
    }
    *out_handle = {};
    try {
        const auto* service_context = context_from(context);
        StreamDescriptorHandle handle;
        const auto result = service_context->state->publish_stream_descriptor(
            service_context->plugin_index, *request, handle);
        if (!result.ok()) {
            return assign_status(result.status(), out_status);
        }
        out_handle->value = handle.value();
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(
            StatusCode::InternalError, "descriptor publication failed", out_status);
    }
}

rodaq_status_code acquisition_report_source(
    void* context, rodaq_source_handle source, rodaq_availability_state availability,
    const rodaq_status*, rodaq_status* out_status) noexcept {
    if (context == nullptr) {
        return assign_message(
            StatusCode::InvalidArgument, "source report context is null", out_status);
    }
    try {
        const auto* service_context = context_from(context);
        const auto result = service_context->state->report_source_availability(
            service_context->plugin_index, SourceHandle::from_raw(source.value), availability);
        return result.ok() ? RODAQ_STATUS_OK : assign_status(result.status(), out_status);
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(StatusCode::InternalError, "source report failed", out_status);
    }
}

rodaq_status_code acquisition_report_stream(
    void* context, rodaq_stream_handle stream, rodaq_availability_state availability,
    const rodaq_status*, rodaq_status* out_status) noexcept {
    if (context == nullptr) {
        return assign_message(
            StatusCode::InvalidArgument, "stream report context is null", out_status);
    }
    try {
        const auto* service_context = context_from(context);
        const auto result = service_context->state->report_stream_availability(
            service_context->plugin_index, StreamHandle::from_raw(stream.value), availability);
        return result.ok() ? RODAQ_STATUS_OK : assign_status(result.status(), out_status);
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(StatusCode::InternalError, "stream report failed", out_status);
    }
}

rodaq_status_code acquisition_report_plugin_state(
    void* context, rodaq_acquisition_plugin_state plugin_state, const rodaq_status* diagnostic,
    rodaq_status* out_status) noexcept {
    if (context == nullptr || (plugin_state != RODAQ_ACQUISITION_PLUGIN_AVAILABLE &&
                               plugin_state != RODAQ_ACQUISITION_PLUGIN_RECOVERABLE_FAILURE &&
                               plugin_state != RODAQ_ACQUISITION_PLUGIN_TERMINAL_FAILURE)) {
        return assign_message(
            StatusCode::InvalidArgument, "Plugin state report is invalid", out_status);
    }
    if (diagnostic != nullptr &&
        (!is_valid_abi_status(*diagnostic) || (plugin_state != RODAQ_ACQUISITION_PLUGIN_AVAILABLE &&
                                               diagnostic->status_code == RODAQ_STATUS_OK))) {
        return assign_message(
            StatusCode::InvalidArgument, "Plugin state diagnostic is malformed", out_status);
    }
    try {
        const auto* service_context = context_from(context);
        const auto result = service_context->state->report_plugin_state(
            service_context->plugin_index, plugin_state, diagnostic);
        return result.ok() ? RODAQ_STATUS_OK : assign_status(result.status(), out_status);
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(StatusCode::InternalError, "Plugin state report failed", out_status);
    }
}

rodaq_status_code acquisition_submit(
    void* context, const rodaq_record_submission* submission,
    rodaq_acquisition_submit_result* out_result, rodaq_status* out_status) noexcept {
    if (context == nullptr || submission == nullptr || out_result == nullptr) {
        return assign_message(StatusCode::InvalidArgument, "submit input is null", out_status);
    }
    const std::uint32_t capacity = out_result->struct_size;
    if (capacity < RODAQ_ACQUISITION_SUBMIT_RESULT_MIN_STRUCT_SIZE) {
        return assign_message(
            StatusCode::InvalidArgument, "submit result capacity is too small", out_status);
    }
    try {
        const auto* service_context = context_from(context);
        rodaq_payload_disposition disposition = RODAQ_PAYLOAD_DISPOSITION_NOT_APPLICABLE;
        const auto result =
            service_context->state->submit(service_context->plugin_index, *submission, disposition);
        if (!result.ok()) {
            return assign_status(result.status(), out_status);
        }
        const auto copied = write_output_prefix(result.value(), out_result);
        return copied.ok() ? RODAQ_STATUS_OK : assign_status(copied.status(), out_status);
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(StatusCode::InternalError, "submit failed", out_status);
    }
}

}  // namespace

const rodaq_runtime_acquisition ActivationState::kAcquisitionVtable = {
    RODAQ_RUNTIME_ACQUISITION_PROTOCOL_VERSION,
    RODAQ_RUNTIME_ACQUISITION_MIN_STRUCT_SIZE,
    &acquisition_register_source,
    &acquisition_register_clock,
    &acquisition_register_schema,
    &acquisition_register_stream,
    &acquisition_publish_descriptor,
    &acquisition_report_source,
    &acquisition_report_stream,
    &acquisition_report_plugin_state,
    &acquisition_submit,
};

}  // namespace rodaq
