#include <rodaq/plugin/runtime_services.hpp>

#include <utility>

namespace rodaq {

bool CommonRuntimeService::valid() const noexcept {
    return context_ != nullptr && table_ != nullptr;
}

Result<CommonRuntimeService> CommonRuntimeService::from(InterfaceView interface) {
    if (!interface.valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "sdk", 0, "Common Runtime interface is incomplete"));
    }
    const auto* table = static_cast<const rodaq_runtime_common_v1*>(interface.v_table());
    if (table->protocol_version != RODAQ_RUNTIME_COMMON_PROTOCOL_VERSION ||
        table->struct_size < RODAQ_RUNTIME_COMMON_V1_MIN_STRUCT_SIZE || table->log == nullptr ||
        table->monotonic_time_ns == nullptr || table->get_lifecycle_snapshot == nullptr ||
        table->report_diagnostic == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "sdk", 0, "Common Runtime ABI is incompatible"));
    }
    return CommonRuntimeService(interface.context(), table);
}

void CommonRuntimeService::log(
    rodaq_log_level level, std::string_view category, std::string_view message) const {
    if (!valid()) {
        return;
    }
    table_->log(
        context_, level, rodaq_string_view{category.data(), category.size()},
        rodaq_string_view{message.data(), message.size()});
}

Result<std::uint64_t> CommonRuntimeService::monotonic_time_ns() const {
    if (!valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "sdk", 0, "Common Runtime service is invalid"));
    }
    return table_->monotonic_time_ns(context_);
}

Result<rodaq_runtime_lifecycle_snapshot> CommonRuntimeService::lifecycle_snapshot() const {
    if (!valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "sdk", 0, "Common Runtime service is invalid"));
    }
    rodaq_runtime_lifecycle_snapshot value{};
    rodaq_error error{};
    const rodaq_status status = table_->get_lifecycle_snapshot(context_, &value, &error);
    if (status != RODAQ_STATUS_OK) {
        return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error());
    }
    return value;
}

void CommonRuntimeService::diagnostic(
    rodaq_diagnostic_severity severity, std::string_view code, std::string_view message) const {
    if (!valid()) {
        return;
    }
    table_->report_diagnostic(
        context_, severity, rodaq_string_view{code.data(), code.size()},
        rodaq_string_view{message.data(), message.size()});
}

CommonRuntimeService::CommonRuntimeService(
    void* context, const rodaq_runtime_common_v1* table) noexcept
: context_(context), table_(table) {}

bool CanonicalRuntimeService::valid() const noexcept {
    return context_ != nullptr && table_ != nullptr;
}

Result<CanonicalRuntimeService> CanonicalRuntimeService::from(InterfaceView interface) {
    if (!interface.valid()) {
        return nonstd::make_unexpected(Error(
            ErrorCode::InvalidArgument, "sdk", 0, "Canonical Runtime interface is incomplete"));
    }
    const auto* table = static_cast<const rodaq_runtime_canonical_v1*>(interface.v_table());
    if (table->protocol_version != RODAQ_RUNTIME_CANONICAL_PROTOCOL_VERSION ||
        table->struct_size < RODAQ_RUNTIME_CANONICAL_V1_MIN_STRUCT_SIZE ||
        table->get_source == nullptr || table->get_clock == nullptr ||
        table->get_schema == nullptr || table->get_timestamp_key_set == nullptr ||
        table->get_stream == nullptr || table->get_stream_descriptor == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "sdk", 0, "Canonical Runtime ABI is incompatible"));
    }
    return CanonicalRuntimeService(interface.context(), table);
}

Result<rodaq_timestamp_key_set_view> CanonicalRuntimeService::get_timestamp_key_set(
    std::uint64_t id) const {
    if (!valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "sdk", 0, "Canonical service is invalid"));
    }
    rodaq_timestamp_key_set_view value{};
    value.struct_size = sizeof(value);
    rodaq_error error{};
    const rodaq_status status = table_->get_timestamp_key_set(context_, id, &value, &error);
    if (status != RODAQ_STATUS_OK) {
        return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error());
    }
    return value;
}

CanonicalRuntimeService::CanonicalRuntimeService(
    void* context, const rodaq_runtime_canonical_v1* table) noexcept
: context_(context), table_(table) {}

bool ProjectionRuntimeService::valid() const noexcept {
    return context_ != nullptr && table_ != nullptr;
}

Result<ProjectionRuntimeService> ProjectionRuntimeService::from(InterfaceView interface) {
    if (!interface.valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "sdk", 0, "Projection interface is incomplete"));
    }
    const auto* table = static_cast<const rodaq_runtime_projection_v1*>(interface.v_table());
    if (table->protocol_version != RODAQ_RUNTIME_PROJECTION_PROTOCOL_VERSION ||
        table->struct_size < RODAQ_RUNTIME_PROJECTION_V1_MIN_STRUCT_SIZE ||
        table->register_projection == nullptr || table->get_projection == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "sdk", 0, "Projection ABI is incompatible"));
    }
    return ProjectionRuntimeService(interface.context(), table);
}

Result<void> ProjectionRuntimeService::register_entry(
    rodaq_stream_descriptor_handle descriptor, std::string_view id, const std::uint8_t* data,
    std::size_t size) const {
    if (!valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "sdk", 0, "Projection service is invalid"));
    }
    if ((!id.empty() && id.data() == nullptr) || (size != 0 && data == nullptr)) {
        return nonstd::make_unexpected(Error(
            ErrorCode::InvalidArgument, "sdk", 0, "Projection registration has an invalid view"));
    }
    rodaq_projection_registration request{};
    request.struct_size = sizeof(request);
    request.descriptor = descriptor;
    request.projection_id = rodaq_string_view{id.data(), id.size()};
    request.payload = rodaq_bytes_view{data, size};
    rodaq_error error{};
    return runtime_result(table_->register_projection(context_, &request, &error), error);
}

Result<rodaq_projection_view> ProjectionRuntimeService::get(
    rodaq_stream_descriptor_handle descriptor, std::string_view id) const {
    if (!valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "sdk", 0, "Projection service is invalid"));
    }
    if (!id.empty() && id.data() == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "sdk", 0, "Projection lookup key is invalid"));
    }
    rodaq_projection_view value{};
    value.struct_size = sizeof(value);
    rodaq_error error{};
    const rodaq_status status = table_->get_projection(
        context_, descriptor, rodaq_string_view{id.data(), id.size()}, &value, &error);
    if (status != RODAQ_STATUS_OK) {
        return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error());
    }
    return value;
}

ProjectionRuntimeService::ProjectionRuntimeService(
    void* context, const rodaq_runtime_projection_v1* table) noexcept
: context_(context), table_(table) {}

bool ClockCorrelationRuntimeService::valid() const noexcept {
    return context_ != nullptr && table_ != nullptr;
}

Result<ClockCorrelationRuntimeService> ClockCorrelationRuntimeService::from(
    InterfaceView interface) {
    if (!interface.valid()) {
        return nonstd::make_unexpected(Error(
            ErrorCode::InvalidArgument, "sdk", 0, "Clock-correlation interface is incomplete"));
    }
    const auto* table = static_cast<const rodaq_runtime_clock_correlation_v1*>(interface.v_table());
    if (table->protocol_version != RODAQ_RUNTIME_CLOCK_CORRELATION_PROTOCOL_VERSION ||
        table->struct_size < RODAQ_RUNTIME_CLOCK_CORRELATION_V1_MIN_STRUCT_SIZE ||
        table->get_unix_epoch_anchor == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "sdk", 0, "Clock-correlation ABI is incompatible"));
    }
    return ClockCorrelationRuntimeService(interface.context(), table);
}

Result<rodaq_unix_epoch_clock_anchor> ClockCorrelationRuntimeService::anchor(
    rodaq_clock_handle clock) const {
    if (!valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "sdk", 0, "Clock service is invalid"));
    }
    rodaq_unix_epoch_clock_anchor value{};
    value.struct_size = sizeof(value);
    rodaq_error error{};
    const rodaq_status status = table_->get_unix_epoch_anchor(context_, clock, &value, &error);
    if (status != RODAQ_STATUS_OK) {
        return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error());
    }
    return value;
}

ClockCorrelationRuntimeService::ClockCorrelationRuntimeService(
    void* context, const rodaq_runtime_clock_correlation_v1* table) noexcept
: context_(context), table_(table) {}

bool AcquisitionRuntimeService::valid() const noexcept {
    return context_ != nullptr && table_ != nullptr;
}

Result<AcquisitionRuntimeService> AcquisitionRuntimeService::from(InterfaceView interface) {
    if (!interface.valid()) {
        return nonstd::make_unexpected(Error(
            ErrorCode::InvalidArgument, "sdk", 0, "Acquisition Runtime interface is incomplete"));
    }
    const auto* table = static_cast<const rodaq_runtime_acquisition_v1*>(interface.v_table());
    if (table->protocol_version != RODAQ_RUNTIME_ACQUISITION_PROTOCOL_VERSION ||
        table->struct_size < RODAQ_RUNTIME_ACQUISITION_V1_MIN_STRUCT_SIZE ||
        table->register_source == nullptr || table->register_clock == nullptr ||
        table->register_schema == nullptr || table->register_stream == nullptr ||
        table->publish_stream_descriptor == nullptr ||
        table->report_source_availability == nullptr ||
        table->report_stream_availability == nullptr || table->report_plugin_state == nullptr ||
        table->submit == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "sdk", 0, "Acquisition Runtime ABI is incompatible"));
    }
    return AcquisitionRuntimeService(interface.context(), table);
}

Result<rodaq_acquisition_submit_result> AcquisitionRuntimeService::submit(
    const rodaq_record_submission& submission) const {
    if (!valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "sdk", 0, "Acquisition service is invalid"));
    }
    if (!plugin::is_valid_record_submission(submission)) {
        return nonstd::make_unexpected(Error(
            ErrorCode::InvalidArgument, "sdk", 0,
            "Acquisition submission is structurally invalid"));
    }
    rodaq_acquisition_submit_result result{};
    result.struct_size = sizeof(result);
    rodaq_error error{};
    const rodaq_status status = table_->submit(context_, &submission, &result, &error);
    if (status != RODAQ_STATUS_OK) {
        return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error());
    }
    return result;
}

Result<void> AcquisitionRuntimeService::report_source(
    rodaq_source_handle source, rodaq_availability_state state,
    const rodaq_error* diagnostic) const {
    if (!valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "sdk", 0, "Acquisition service is invalid"));
    }
    rodaq_error error{};
    return runtime_result(
        table_->report_source_availability(context_, source, state, diagnostic, &error), error);
}

Result<void> AcquisitionRuntimeService::report_stream(
    rodaq_stream_handle stream, rodaq_availability_state state,
    const rodaq_error* diagnostic) const {
    if (!valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "sdk", 0, "Acquisition service is invalid"));
    }
    rodaq_error error{};
    return runtime_result(
        table_->report_stream_availability(context_, stream, state, diagnostic, &error), error);
}

Result<void> AcquisitionRuntimeService::report_plugin_state(
    rodaq_acquisition_plugin_state state, const rodaq_error* diagnostic) const {
    if (!valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "sdk", 0, "Acquisition service is invalid"));
    }
    rodaq_error error{};
    return runtime_result(table_->report_plugin_state(context_, state, diagnostic, &error), error);
}

AcquisitionRuntimeService::AcquisitionRuntimeService(
    void* context, const rodaq_runtime_acquisition_v1* table) noexcept
: context_(context), table_(table) {}

bool StorageRuntimeService::valid() const noexcept {
    return context_ != nullptr && table_ != nullptr;
}

Result<StorageRuntimeService> StorageRuntimeService::from(InterfaceView interface) {
    if (!interface.valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "sdk", 0, "Storage Runtime interface is incomplete"));
    }
    const auto* table = static_cast<const rodaq_runtime_storage_v1*>(interface.v_table());
    if (table->protocol_version != RODAQ_RUNTIME_STORAGE_PROTOCOL_VERSION ||
        table->struct_size < RODAQ_RUNTIME_STORAGE_V1_MIN_STRUCT_SIZE ||
        table->payload_consumed == nullptr || table->persistence_completed == nullptr ||
        table->report_failure == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "sdk", 0, "Storage Runtime ABI is incompatible"));
    }
    return StorageRuntimeService(interface.context(), table);
}

void StorageRuntimeService::payload_consumed(rodaq_storage_operation operation) const {
    if (!valid()) {
        return;
    }
    table_->payload_consumed(context_, operation);
}

void StorageRuntimeService::persistence_completed(
    rodaq_storage_operation operation, rodaq_storage_persistence_state state,
    const rodaq_error* error) const {
    if (!valid()) {
        return;
    }
    table_->persistence_completed(context_, operation, state, error);
}

void StorageRuntimeService::report_failure(const rodaq_error* error) const {
    if (!valid()) {
        return;
    }
    table_->report_failure(context_, error);
}

StorageRuntimeService::StorageRuntimeService(
    void* context, const rodaq_runtime_storage_v1* table) noexcept
: context_(context), table_(table) {}

Result<AcquisitionRuntime> AcquisitionRuntime::bind(ServiceRegistryView services) {
    Result<InterfaceView> common_interface = services.query(RODAQ_RUNTIME_COMMON_SERVICE_ID);
    if (!common_interface.has_value()) {
        return nonstd::make_unexpected(std::move(common_interface.error()));
    }
    auto common = CommonRuntimeService::from(common_interface.value());
    if (!common.has_value()) {
        return nonstd::make_unexpected(std::move(common.error()));
    }
    Result<InterfaceView> canonical_interface = services.query(RODAQ_RUNTIME_CANONICAL_SERVICE_ID);
    if (!canonical_interface.has_value()) {
        return nonstd::make_unexpected(std::move(canonical_interface.error()));
    }
    auto canonical = CanonicalRuntimeService::from(canonical_interface.value());
    if (!canonical.has_value()) {
        return nonstd::make_unexpected(std::move(canonical.error()));
    }
    Result<InterfaceView> projection_interface =
        services.query(RODAQ_RUNTIME_PROJECTION_SERVICE_ID);
    if (!projection_interface.has_value()) {
        return nonstd::make_unexpected(std::move(projection_interface.error()));
    }
    auto projection = ProjectionRuntimeService::from(projection_interface.value());
    if (!projection.has_value()) {
        return nonstd::make_unexpected(std::move(projection.error()));
    }
    Result<InterfaceView> acquisition_interface =
        services.query(RODAQ_RUNTIME_ACQUISITION_SERVICE_ID);
    if (!acquisition_interface.has_value()) {
        return nonstd::make_unexpected(std::move(acquisition_interface.error()));
    }
    auto acquisition = AcquisitionRuntimeService::from(acquisition_interface.value());
    if (!acquisition.has_value()) {
        return nonstd::make_unexpected(std::move(acquisition.error()));
    }
    return AcquisitionRuntime{
        std::move(common.value()),
        std::move(canonical.value()),
        std::move(projection.value()),
        std::move(acquisition.value()),
    };
}

Result<StorageRuntime> StorageRuntime::bind(ServiceRegistryView services) {
    Result<InterfaceView> common_interface = services.query(RODAQ_RUNTIME_COMMON_SERVICE_ID);
    if (!common_interface.has_value()) {
        return nonstd::make_unexpected(std::move(common_interface.error()));
    }
    auto common = CommonRuntimeService::from(common_interface.value());
    if (!common.has_value()) {
        return nonstd::make_unexpected(std::move(common.error()));
    }
    Result<InterfaceView> canonical_interface = services.query(RODAQ_RUNTIME_CANONICAL_SERVICE_ID);
    if (!canonical_interface.has_value()) {
        return nonstd::make_unexpected(std::move(canonical_interface.error()));
    }
    auto canonical = CanonicalRuntimeService::from(canonical_interface.value());
    if (!canonical.has_value()) {
        return nonstd::make_unexpected(std::move(canonical.error()));
    }
    Result<InterfaceView> projection_interface =
        services.query(RODAQ_RUNTIME_PROJECTION_SERVICE_ID);
    if (!projection_interface.has_value()) {
        return nonstd::make_unexpected(std::move(projection_interface.error()));
    }
    auto projection = ProjectionRuntimeService::from(projection_interface.value());
    if (!projection.has_value()) {
        return nonstd::make_unexpected(std::move(projection.error()));
    }
    Result<InterfaceView> clock_interface =
        services.query(RODAQ_RUNTIME_CLOCK_CORRELATION_SERVICE_ID);
    if (!clock_interface.has_value()) {
        return nonstd::make_unexpected(std::move(clock_interface.error()));
    }
    auto clock = ClockCorrelationRuntimeService::from(clock_interface.value());
    if (!clock.has_value()) {
        return nonstd::make_unexpected(std::move(clock.error()));
    }
    Result<InterfaceView> storage_interface = services.query(RODAQ_RUNTIME_STORAGE_SERVICE_ID);
    if (!storage_interface.has_value()) {
        return nonstd::make_unexpected(std::move(storage_interface.error()));
    }
    auto storage = StorageRuntimeService::from(storage_interface.value());
    if (!storage.has_value()) {
        return nonstd::make_unexpected(std::move(storage.error()));
    }
    return StorageRuntime{
        std::move(common.value()), std::move(canonical.value()), std::move(projection.value()),
        std::move(clock.value()),  std::move(storage.value()),
    };
}

#define RODAQ_SDK_CANONICAL_GETTER(method, handle_type, view_type)                             \
    Result<view_type> CanonicalRuntimeService::method(handle_type handle) const {              \
        if (!valid()) {                                                                        \
            return nonstd::make_unexpected(                                                    \
                Error(ErrorCode::InvalidState, "sdk", 0, "Canonical service is invalid"));     \
        }                                                                                      \
        view_type value{};                                                                     \
        value.struct_size = sizeof(view_type);                                                 \
        rodaq_error error{};                                                                   \
        const rodaq_status status = table_->method(context_, handle, &value, &error);          \
        if (status != RODAQ_STATUS_OK) {                                                       \
            return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error()); \
        }                                                                                      \
        return value;                                                                          \
    }

RODAQ_SDK_CANONICAL_GETTER(get_source, rodaq_source_handle, rodaq_source_view)
RODAQ_SDK_CANONICAL_GETTER(get_clock, rodaq_clock_handle, rodaq_clock_view)
RODAQ_SDK_CANONICAL_GETTER(get_schema, rodaq_schema_handle, rodaq_schema_view)
RODAQ_SDK_CANONICAL_GETTER(get_stream, rodaq_stream_handle, rodaq_stream_view)
RODAQ_SDK_CANONICAL_GETTER(
    get_stream_descriptor, rodaq_stream_descriptor_handle, rodaq_stream_descriptor_view)
#undef RODAQ_SDK_CANONICAL_GETTER

#define RODAQ_SDK_ACQ_REGISTER(method, abi_method, request_type, handle_type, validator)       \
    Result<handle_type> AcquisitionRuntimeService::method(const request_type& request) const { \
        if (!valid()) {                                                                        \
            return nonstd::make_unexpected(                                                    \
                Error(ErrorCode::InvalidState, "sdk", 0, "Acquisition service is invalid"));   \
        }                                                                                      \
        if (!plugin::validator(request)) {                                                     \
            return nonstd::make_unexpected(Error(                                              \
                ErrorCode::InvalidArgument, "sdk", 0,                                          \
                "Acquisition registration request is structurally invalid"));                  \
        }                                                                                      \
        handle_type handle{};                                                                  \
        rodaq_error error{};                                                                   \
        const rodaq_status status = table_->abi_method(context_, &request, &handle, &error);   \
        if (status != RODAQ_STATUS_OK) {                                                       \
            return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error()); \
        }                                                                                      \
        return handle;                                                                         \
    }
RODAQ_SDK_ACQ_REGISTER(
    register_source, register_source, rodaq_source_registration, rodaq_source_handle,
    is_valid_source_registration)
RODAQ_SDK_ACQ_REGISTER(
    register_clock, register_clock, rodaq_clock_registration, rodaq_clock_handle,
    is_valid_clock_registration)
RODAQ_SDK_ACQ_REGISTER(
    register_schema, register_schema, rodaq_schema_registration, rodaq_schema_handle,
    is_valid_schema_registration)
RODAQ_SDK_ACQ_REGISTER(
    register_stream, register_stream, rodaq_stream_registration, rodaq_stream_handle,
    is_valid_stream_registration)
RODAQ_SDK_ACQ_REGISTER(
    publish_descriptor, publish_stream_descriptor, rodaq_stream_descriptor_registration,
    rodaq_stream_descriptor_handle, is_valid_stream_descriptor_registration)
#undef RODAQ_SDK_ACQ_REGISTER

}  // namespace rodaq
