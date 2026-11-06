#include "activation/activation_internal.hpp"
#include "plugin/service_registry.hpp"

namespace rodaq {

namespace {

rodaq_status_code assign_status(const Status& error, rodaq_status* out_status) noexcept {
    assign_abi_status(error, out_status);
    return abi_status_from_status_code(error.status_code());
}

rodaq_status_code assign_message(
    StatusCode status_code, std::string_view message, rodaq_status* out_status) noexcept {
    assign_abi_status(abi_status_from_status_code(status_code), "runtime", 0, message, out_status);
    return abi_status_from_status_code(status_code);
}

ActivationState* state_from(void* context) noexcept {
    return static_cast<ActivationState*>(context);
}

const plugin::AcquisitionServiceContext* acquisition_context_from(void* context) noexcept {
    return static_cast<const plugin::AcquisitionServiceContext*>(context);
}

const plugin::StorageServiceContext* storage_context_from(void* context) noexcept {
    return static_cast<const plugin::StorageServiceContext*>(context);
}

void common_log(
    void* context, rodaq_log_level level, rodaq_string_view category,
    rodaq_string_view message) noexcept {
    if (context == nullptr) {
        return;
    }
    auto* state = state_from(context);
    std::string_view parsed_category;
    std::string_view parsed_message;
    if (rodaq::parse_abi_string(category, &parsed_category) &&
        rodaq::parse_abi_string(message, &parsed_message)) {
        state->log(static_cast<LogLevel>(level), parsed_category, parsed_message);
    }
}

std::uint64_t common_monotonic_time_ns(void*) noexcept {
    const auto now = Clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

rodaq_status_code common_lifecycle_snapshot(
    void* context, rodaq_runtime_lifecycle_snapshot* out_snapshot,
    rodaq_status* out_status) noexcept {
    if (context == nullptr || out_snapshot == nullptr) {
        return assign_message(
            StatusCode::InvalidArgument, "lifecycle snapshot input is invalid", out_status);
    }
    try {
        *out_snapshot = {};
        const auto result = state_from(context)->lifecycle_snapshot();
        if (!result.ok()) {
            return assign_status(result.status(), out_status);
        }
        const DAQLifecycleSnapshot& snapshot = result.value();
        switch (snapshot.state) {
            case DAQState::Constructed:
            case DAQState::Configured:
                *out_snapshot = {
                    RODAQ_RUNTIME_CONFIGURED,
                    snapshot.normal_acquisition_allowed ? 1U : 0U,
                    snapshot.generation,
                };
                break;
            case DAQState::Starting:
                *out_snapshot = {
                    RODAQ_RUNTIME_STARTING,
                    snapshot.normal_acquisition_allowed ? 1U : 0U,
                    snapshot.generation,
                };
                break;
            case DAQState::Running:
                *out_snapshot = {
                    RODAQ_RUNTIME_RUNNING,
                    snapshot.normal_acquisition_allowed ? 1U : 0U,
                    snapshot.generation,
                };
                break;
            case DAQState::Stopping:
                *out_snapshot = {
                    RODAQ_RUNTIME_STOPPING,
                    snapshot.normal_acquisition_allowed ? 1U : 0U,
                    snapshot.generation,
                };
                break;
            case DAQState::Stopped:
                *out_snapshot = {
                    RODAQ_RUNTIME_STOPPED,
                    snapshot.normal_acquisition_allowed ? 1U : 0U,
                    snapshot.generation,
                };
                break;
            case DAQState::Failed:
                *out_snapshot = {
                    RODAQ_RUNTIME_FAILED,
                    snapshot.normal_acquisition_allowed ? 1U : 0U,
                    snapshot.generation,
                };
                break;
        }
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(StatusCode::InternalError, "lifecycle snapshot failed", out_status);
    }
}

void common_diagnostic(
    void* context, rodaq_diagnostic_severity severity, rodaq_string_view code,
    rodaq_string_view message) noexcept {
    if (context == nullptr) {
        return;
    }
    std::string_view parsed_code;
    std::string_view parsed_message;
    if (rodaq::parse_abi_string(code, &parsed_code) &&
        rodaq::parse_abi_string(message, &parsed_message)) {
        state_from(context)->diagnostic(severity, parsed_code, parsed_message);
    }
}

template <typename View, typename Getter>
rodaq_status_code canonical_view(
    void* context, View* out_view, rodaq_status* out_status, Getter&& getter) noexcept {
    if (context == nullptr || out_view == nullptr) {
        return assign_message(
            StatusCode::InvalidArgument, "canonical view input is null", out_status);
    }
    try {
        const auto result = getter(*state_from(context));
        if (!result.ok()) {
            return assign_status(result.status(), out_status);
        }
        const auto copied = write_output_prefix(result.value(), out_view);
        return copied.ok() ? RODAQ_STATUS_OK : assign_status(copied.status(), out_status);
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(StatusCode::InternalError, "canonical view failed", out_status);
    }
}

rodaq_status_code canonical_get_source(
    void* context, rodaq_source_handle handle, rodaq_source_view* out_view,
    rodaq_status* out_status) noexcept {
    return canonical_view(context, out_view, out_status, [handle](ActivationState& state) {
        rodaq_source_view view{};
        view.struct_size = sizeof(view);
        auto result = state.get_source(SourceHandle::from_raw(handle.value), view);
        if (!result.ok()) {
            return Result<rodaq_source_view>(std::move(result).status());
        }
        return Result<rodaq_source_view>(view);
    });
}

rodaq_status_code canonical_get_clock(
    void* context, rodaq_clock_handle handle, rodaq_clock_view* out_view,
    rodaq_status* out_status) noexcept {
    return canonical_view(context, out_view, out_status, [handle](ActivationState& state) {
        rodaq_clock_view view{};
        view.struct_size = sizeof(view);
        auto result = state.get_clock(ClockHandle::from_raw(handle.value), view);
        if (!result.ok()) {
            return Result<rodaq_clock_view>(std::move(result).status());
        }
        return Result<rodaq_clock_view>(view);
    });
}

rodaq_status_code canonical_get_schema(
    void* context, rodaq_schema_handle handle, rodaq_schema_view* out_view,
    rodaq_status* out_status) noexcept {
    return canonical_view(context, out_view, out_status, [handle](ActivationState& state) {
        rodaq_schema_view view{};
        view.struct_size = sizeof(view);
        auto result = state.get_schema(SchemaHandle::from_raw(handle.value), view);
        if (!result.ok()) {
            return Result<rodaq_schema_view>(std::move(result).status());
        }
        return Result<rodaq_schema_view>(view);
    });
}

rodaq_status_code canonical_get_timestamp_key_set(
    void* context, std::uint64_t id, rodaq_timestamp_key_set_view* out_view,
    rodaq_status* out_status) noexcept {
    return canonical_view(context, out_view, out_status, [id](ActivationState& state) {
        rodaq_timestamp_key_set_view view{};
        view.struct_size = sizeof(view);
        auto result = state.get_timestamp_key_set(id, view);
        if (!result.ok()) {
            return Result<rodaq_timestamp_key_set_view>(std::move(result).status());
        }
        return Result<rodaq_timestamp_key_set_view>(view);
    });
}

rodaq_status_code canonical_get_stream(
    void* context, rodaq_stream_handle handle, rodaq_stream_view* out_view,
    rodaq_status* out_status) noexcept {
    return canonical_view(context, out_view, out_status, [handle](ActivationState& state) {
        rodaq_stream_view view{};
        view.struct_size = sizeof(view);
        auto result = state.get_stream(StreamHandle::from_raw(handle.value), view);
        if (!result.ok()) {
            return Result<rodaq_stream_view>(std::move(result).status());
        }
        return Result<rodaq_stream_view>(view);
    });
}

rodaq_status_code canonical_get_stream_descriptor(
    void* context, rodaq_stream_descriptor_handle handle, rodaq_stream_descriptor_view* out_view,
    rodaq_status* out_status) noexcept {
    return canonical_view(context, out_view, out_status, [handle](ActivationState& state) {
        rodaq_stream_descriptor_view view{};
        view.struct_size = sizeof(view);
        auto result =
            state.get_stream_descriptor(StreamDescriptorHandle::from_raw(handle.value), view);
        if (!result.ok()) {
            return Result<rodaq_stream_descriptor_view>(std::move(result).status());
        }
        return Result<rodaq_stream_descriptor_view>(view);
    });
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
        auto* service_context = acquisition_context_from(context);
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
        auto* service_context = acquisition_context_from(context);
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
        auto* service_context = acquisition_context_from(context);
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
        auto* service_context = acquisition_context_from(context);
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
        auto* service_context = acquisition_context_from(context);
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
        const auto result = state_from(acquisition_context_from(context)->state)
                                ->report_source_availability(
                                    acquisition_context_from(context)->plugin_index,
                                    SourceHandle::from_raw(source.value), availability);
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
        const auto result = state_from(acquisition_context_from(context)->state)
                                ->report_stream_availability(
                                    acquisition_context_from(context)->plugin_index,
                                    StreamHandle::from_raw(stream.value), availability);
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
            StatusCode::InvalidArgument, "plugin state report is invalid", out_status);
    }
    if (diagnostic != nullptr && (!rodaq::is_valid_abi_status(*diagnostic) ||
                                  (plugin_state != RODAQ_ACQUISITION_PLUGIN_AVAILABLE &&
                                   diagnostic->status_code == RODAQ_STATUS_OK))) {
        return assign_message(
            StatusCode::InvalidArgument, "plugin state diagnostic is malformed", out_status);
    }
    try {
        const auto* service_context = acquisition_context_from(context);
        const auto result =
            state_from(service_context->state)
                ->report_plugin_state(service_context->plugin_index, plugin_state, diagnostic);
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
        auto* service_context = acquisition_context_from(context);
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

void storage_payload_consumed(void* context, rodaq_storage_operation operation) noexcept {
    if (context == nullptr) {
        return;
    }
    try {
        auto* state = state_from(storage_context_from(context)->state);
        if (state->pipeline() != nullptr) {
            state->pipeline()->payload_consumed(operation);
        }
    } catch (...) {
    }
}

void storage_persistence_completed(
    void* context, rodaq_storage_operation operation, rodaq_storage_persistence_state state_value,
    const rodaq_status* error) noexcept {
    if (context == nullptr) {
        return;
    }
    try {
        auto* state = state_from(storage_context_from(context)->state);
        if (error != nullptr &&
            (!rodaq::is_valid_abi_status(*error) || error->status_code == RODAQ_STATUS_OK)) {
            state->request_failure(activation_status(
                StatusCode::InvalidArgument,
                "Storage persistence callback supplied a malformed diagnostic"));
            return;
        }
        if (state->pipeline() != nullptr) {
            state->pipeline()->persistence_completed(operation, state_value, error);
        }
    } catch (...) {
    }
}

void storage_report_failure(void* context, const rodaq_status* error) noexcept {
    if (context == nullptr) {
        return;
    }
    try {
        if (error != nullptr &&
            (!rodaq::is_valid_abi_status(*error) || error->status_code == RODAQ_STATUS_OK)) {
            state_from(storage_context_from(context)->state)
                ->request_failure(activation_status(
                    StatusCode::InvalidArgument,
                    "Storage failure callback supplied a malformed diagnostic"));
            return;
        }
        Status failure =
            error == nullptr
                ? activation_status(StatusCode::StorageError, "Storage reported failure")
                : status_from_abi_diagnostic(*error);
        state_from(storage_context_from(context)->state)->request_failure(std::move(failure));
    } catch (...) {
    }
}

}  // namespace

const rodaq_runtime_common ActivationState::kCommonVtable = {
    RODAQ_RUNTIME_COMMON_PROTOCOL_VERSION,
    RODAQ_RUNTIME_COMMON_MIN_STRUCT_SIZE,
    &common_log,
    &common_monotonic_time_ns,
    &common_lifecycle_snapshot,
    &common_diagnostic,
};

const rodaq_runtime_canonical ActivationState::kCanonicalVtable = {
    RODAQ_RUNTIME_CANONICAL_PROTOCOL_VERSION,
    RODAQ_RUNTIME_CANONICAL_MIN_STRUCT_SIZE,
    &canonical_get_source,
    &canonical_get_clock,
    &canonical_get_schema,
    &canonical_get_timestamp_key_set,
    &canonical_get_stream,
    &canonical_get_stream_descriptor,
};

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

const rodaq_runtime_storage ActivationState::kStorageVtable = {
    RODAQ_RUNTIME_STORAGE_PROTOCOL_VERSION,
    RODAQ_RUNTIME_STORAGE_MIN_STRUCT_SIZE,
    &storage_payload_consumed,
    &storage_persistence_completed,
    &storage_report_failure,
};

Result<void> ActivationState::register_acquisition_services(
    plugin::ServiceRegistry& registry, std::size_t plugin_index) {
    if (plugin_index >= acquisition_contexts_.size()) {
        return activation_status(
            StatusCode::InvalidArgument, "Acquisition service index is invalid");
    }
    auto result = registry.register_entry(
        RODAQ_RUNTIME_COMMON_SERVICE_ID, rodaq_interface{this, &kCommonVtable});
    if (!result.ok()) {
        return result;
    }
    result = registry.register_entry(
        RODAQ_RUNTIME_CANONICAL_SERVICE_ID, rodaq_interface{this, &kCanonicalVtable});
    if (!result.ok()) {
        return result;
    }
    result =
        registry.register_entry(RODAQ_RUNTIME_PROJECTION_SERVICE_ID, projections_.abi_interface());
    if (!result.ok()) {
        return result;
    }
    result = registry.register_entry(
        RODAQ_RUNTIME_ACQUISITION_SERVICE_ID, acquisition_service_interface(plugin_index));
    return result;
}

Result<void> ActivationState::register_storage_services(plugin::ServiceRegistry& registry) {
    auto result = registry.register_entry(
        RODAQ_RUNTIME_COMMON_SERVICE_ID, rodaq_interface{this, &kCommonVtable});
    if (!result.ok()) {
        return result;
    }
    result = registry.register_entry(
        RODAQ_RUNTIME_CANONICAL_SERVICE_ID, rodaq_interface{this, &kCanonicalVtable});
    if (!result.ok()) {
        return result;
    }
    result =
        registry.register_entry(RODAQ_RUNTIME_PROJECTION_SERVICE_ID, projections_.abi_interface());
    if (!result.ok()) {
        return result;
    }
    if (!ingress_clock_correlation_.has_value()) {
        return activation_status(StatusCode::InvalidState, "DAQ ingress Clock is unavailable");
    }
    result = registry.register_entry(
        RODAQ_RUNTIME_CLOCK_CORRELATION_SERVICE_ID, ingress_clock_correlation_->abi_interface());
    if (!result.ok()) {
        return result;
    }
    result = registry.register_entry(RODAQ_RUNTIME_STORAGE_SERVICE_ID, storage_service_interface());
    return result;
}

rodaq_interface ActivationState::acquisition_service_interface(std::size_t plugin_index) noexcept {
    return rodaq_interface{
        acquisition_contexts_[plugin_index].get(),
        &kAcquisitionVtable,
    };
}

rodaq_interface ActivationState::storage_service_interface() noexcept {
    return rodaq_interface{
        storage_context_.get(),
        &kStorageVtable,
    };
}

}  // namespace rodaq
