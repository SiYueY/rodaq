#include <rodaq/plugin/services.hpp>

#include <utility>

namespace rodaq::plugin {

bool CommonService::valid() const noexcept { return context_ != nullptr && table_ != nullptr; }

Result<CommonService> CommonService::from(InterfaceRef interface) {
    if (!interface.valid()) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Common DAQ interface is incomplete");
    }
    const auto* table = static_cast<const rodaq_runtime_common*>(interface.v_table());
    if (table->protocol_version != RODAQ_RUNTIME_COMMON_PROTOCOL_VERSION ||
        table->struct_size < RODAQ_RUNTIME_COMMON_MIN_STRUCT_SIZE || table->log == nullptr ||
        table->monotonic_time_ns == nullptr || table->get_lifecycle_snapshot == nullptr ||
        table->report_diagnostic == nullptr) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Common DAQ ABI is incompatible");
    }
    return CommonService(interface.context(), table);
}

void CommonService::log(
    rodaq_log_level level, std::string_view category, std::string_view message) const {
    if (!valid()) {
        return;
    }
    table_->log(
        context_, level, rodaq_string_view{category.data(), category.size()},
        rodaq_string_view{message.data(), message.size()});
}

Result<std::uint64_t> CommonService::monotonic_time_ns() const {
    if (!valid()) {
        return rodaq::Status(StatusCode::InvalidState, "sdk", 0, "Common DAQ service is invalid");
    }
    return table_->monotonic_time_ns(context_);
}

Result<rodaq_runtime_lifecycle_snapshot> CommonService::lifecycle_snapshot() const {
    if (!valid()) {
        return rodaq::Status(StatusCode::InvalidState, "sdk", 0, "Common DAQ service is invalid");
    }
    rodaq_runtime_lifecycle_snapshot snapshot{};
    rodaq_status error{};
    const rodaq_status_code status = table_->get_lifecycle_snapshot(context_, &snapshot, &error);
    if (status != RODAQ_STATUS_OK) {
        return status_from_abi(status, &error, "sdk");
    }
    return snapshot;
}

void CommonService::diagnostic(
    rodaq_diagnostic_severity severity, std::string_view code, std::string_view message) const {
    if (!valid()) {
        return;
    }
    table_->report_diagnostic(
        context_, severity, rodaq_string_view{code.data(), code.size()},
        rodaq_string_view{message.data(), message.size()});
}

CommonService::CommonService(void* context, const rodaq_runtime_common* table) noexcept
: context_(context), table_(table) {}

bool CanonicalService::valid() const noexcept { return context_ != nullptr && table_ != nullptr; }

Result<CanonicalService> CanonicalService::from(InterfaceRef interface) {
    if (!interface.valid()) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Canonical DAQ interface is incomplete");
    }
    const auto* table = static_cast<const rodaq_runtime_canonical*>(interface.v_table());
    if (table->protocol_version != RODAQ_RUNTIME_CANONICAL_PROTOCOL_VERSION ||
        table->struct_size < RODAQ_RUNTIME_CANONICAL_MIN_STRUCT_SIZE ||
        table->get_source == nullptr || table->get_clock == nullptr ||
        table->get_schema == nullptr || table->get_timestamp_key_set == nullptr ||
        table->get_stream == nullptr || table->get_stream_descriptor == nullptr) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Canonical DAQ ABI is incompatible");
    }
    return CanonicalService(interface.context(), table);
}

Result<rodaq_timestamp_key_set_info> CanonicalService::get_timestamp_key_set(
    std::uint64_t id) const {
    if (!valid()) {
        return rodaq::Status(StatusCode::InvalidState, "sdk", 0, "Canonical service is invalid");
    }
    rodaq_timestamp_key_set_info key_set{};
    key_set.struct_size = sizeof(key_set);
    rodaq_status error{};
    const rodaq_status_code status = table_->get_timestamp_key_set(context_, id, &key_set, &error);
    if (status != RODAQ_STATUS_OK) {
        return status_from_abi(status, &error, "sdk");
    }
    return key_set;
}

CanonicalService::CanonicalService(void* context, const rodaq_runtime_canonical* table) noexcept
: context_(context), table_(table) {}

bool ProjectionService::valid() const noexcept { return context_ != nullptr && table_ != nullptr; }

Result<ProjectionService> ProjectionService::from(InterfaceRef interface) {
    if (!interface.valid()) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Projection interface is incomplete");
    }
    const auto* table = static_cast<const rodaq_runtime_projection*>(interface.v_table());
    if (table->protocol_version != RODAQ_RUNTIME_PROJECTION_PROTOCOL_VERSION ||
        table->struct_size < RODAQ_RUNTIME_PROJECTION_MIN_STRUCT_SIZE ||
        table->register_projection == nullptr || table->get_projection == nullptr) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Projection ABI is incompatible");
    }
    return ProjectionService(interface.context(), table);
}

Result<void> ProjectionService::register_entry(
    rodaq_stream_descriptor_handle descriptor, std::string_view id, const std::uint8_t* data,
    std::size_t size) const {
    if (!valid()) {
        return rodaq::Status(StatusCode::InvalidState, "sdk", 0, "Projection service is invalid");
    }
    if ((!id.empty() && id.data() == nullptr) || (size != 0 && data == nullptr)) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Projection registration has an invalid view");
    }
    rodaq_projection_registration request{};
    request.struct_size = sizeof(request);
    request.descriptor = descriptor;
    request.projection_id = rodaq_string_view{id.data(), id.size()};
    request.payload = rodaq_bytes_view{data, size};
    rodaq_status error{};
    return runtime_result(table_->register_projection(context_, &request, &error), error);
}

Result<rodaq_projection_info> ProjectionService::get(
    rodaq_stream_descriptor_handle descriptor, std::string_view id) const {
    if (!valid()) {
        return rodaq::Status(StatusCode::InvalidState, "sdk", 0, "Projection service is invalid");
    }
    if (!id.empty() && id.data() == nullptr) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Projection lookup key is invalid");
    }
    rodaq_projection_info projection{};
    projection.struct_size = sizeof(projection);
    rodaq_status error{};
    const rodaq_status_code status = table_->get_projection(
        context_, descriptor, rodaq_string_view{id.data(), id.size()}, &projection, &error);
    if (status != RODAQ_STATUS_OK) {
        return status_from_abi(status, &error, "sdk");
    }
    return projection;
}

ProjectionService::ProjectionService(void* context, const rodaq_runtime_projection* table) noexcept
: context_(context), table_(table) {}

bool ClockCorrelationService::valid() const noexcept {
    return context_ != nullptr && table_ != nullptr;
}

Result<ClockCorrelationService> ClockCorrelationService::from(InterfaceRef interface) {
    if (!interface.valid()) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Clock-correlation interface is incomplete");
    }
    const auto* table = static_cast<const rodaq_runtime_clock_correlation*>(interface.v_table());
    if (table->protocol_version != RODAQ_RUNTIME_CLOCK_CORRELATION_PROTOCOL_VERSION ||
        table->struct_size < RODAQ_RUNTIME_CLOCK_CORRELATION_MIN_STRUCT_SIZE ||
        table->get_unix_epoch_anchor == nullptr) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Clock-correlation ABI is incompatible");
    }
    return ClockCorrelationService(interface.context(), table);
}

Result<rodaq_unix_epoch_clock_anchor> ClockCorrelationService::anchor(
    rodaq_clock_handle clock) const {
    if (!valid()) {
        return rodaq::Status(StatusCode::InvalidState, "sdk", 0, "Clock service is invalid");
    }
    rodaq_unix_epoch_clock_anchor value{};
    value.struct_size = sizeof(value);
    rodaq_status error{};
    const rodaq_status_code status = table_->get_unix_epoch_anchor(context_, clock, &value, &error);
    if (status != RODAQ_STATUS_OK) {
        return status_from_abi(status, &error, "sdk");
    }
    return value;
}

ClockCorrelationService::ClockCorrelationService(
    void* context, const rodaq_runtime_clock_correlation* table) noexcept
: context_(context), table_(table) {}

bool AcquisitionService::valid() const noexcept { return context_ != nullptr && table_ != nullptr; }

Result<AcquisitionService> AcquisitionService::from(InterfaceRef interface) {
    if (!interface.valid()) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Acquisition DAQ interface is incomplete");
    }
    const auto* table = static_cast<const rodaq_runtime_acquisition*>(interface.v_table());
    if (table->protocol_version != RODAQ_RUNTIME_ACQUISITION_PROTOCOL_VERSION ||
        table->struct_size < RODAQ_RUNTIME_ACQUISITION_MIN_STRUCT_SIZE ||
        table->register_source == nullptr || table->register_clock == nullptr ||
        table->register_schema == nullptr || table->register_stream == nullptr ||
        table->publish_stream_descriptor == nullptr ||
        table->report_source_availability == nullptr ||
        table->report_stream_availability == nullptr || table->report_plugin_state == nullptr ||
        table->submit == nullptr) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Acquisition DAQ ABI is incompatible");
    }
    return AcquisitionService(interface.context(), table);
}

Result<rodaq_acquisition_submit_result> AcquisitionService::submit(
    const rodaq_record_submission& submission) const {
    if (!valid()) {
        return rodaq::Status(StatusCode::InvalidState, "sdk", 0, "Acquisition service is invalid");
    }
    if (!plugin::is_valid_record_submission(submission)) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0,
            "Acquisition submission is structurally invalid");
    }
    rodaq_acquisition_submit_result result{};
    result.struct_size = sizeof(result);
    rodaq_status error{};
    const rodaq_status_code status = table_->submit(context_, &submission, &result, &error);
    if (status != RODAQ_STATUS_OK) {
        return status_from_abi(status, &error, "sdk");
    }
    return result;
}

Result<void> AcquisitionService::report_source(
    rodaq_source_handle source, rodaq_availability_state state,
    const rodaq_status* diagnostic) const {
    if (!valid()) {
        return rodaq::Status(StatusCode::InvalidState, "sdk", 0, "Acquisition service is invalid");
    }
    rodaq_status error{};
    return runtime_result(
        table_->report_source_availability(context_, source, state, diagnostic, &error), error);
}

Result<void> AcquisitionService::report_stream(
    rodaq_stream_handle stream, rodaq_availability_state state,
    const rodaq_status* diagnostic) const {
    if (!valid()) {
        return rodaq::Status(StatusCode::InvalidState, "sdk", 0, "Acquisition service is invalid");
    }
    rodaq_status error{};
    return runtime_result(
        table_->report_stream_availability(context_, stream, state, diagnostic, &error), error);
}

Result<void> AcquisitionService::report_plugin_state(
    rodaq_acquisition_plugin_state state, const rodaq_status* diagnostic) const {
    if (!valid()) {
        return rodaq::Status(StatusCode::InvalidState, "sdk", 0, "Acquisition service is invalid");
    }
    rodaq_status error{};
    return runtime_result(table_->report_plugin_state(context_, state, diagnostic, &error), error);
}

AcquisitionService::AcquisitionService(
    void* context, const rodaq_runtime_acquisition* table) noexcept
: context_(context), table_(table) {}

bool StorageService::valid() const noexcept { return context_ != nullptr && table_ != nullptr; }

Result<StorageService> StorageService::from(InterfaceRef interface) {
    if (!interface.valid()) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Storage DAQ interface is incomplete");
    }
    const auto* table = static_cast<const rodaq_runtime_storage*>(interface.v_table());
    if (table->protocol_version != RODAQ_RUNTIME_STORAGE_PROTOCOL_VERSION ||
        table->struct_size < RODAQ_RUNTIME_STORAGE_MIN_STRUCT_SIZE ||
        table->payload_consumed == nullptr || table->persistence_completed == nullptr ||
        table->report_failure == nullptr) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "Storage DAQ ABI is incompatible");
    }
    return StorageService(interface.context(), table);
}

void StorageService::payload_consumed(rodaq_storage_operation operation) const {
    if (!valid()) {
        return;
    }
    table_->payload_consumed(context_, operation);
}

void StorageService::persistence_completed(
    rodaq_storage_operation operation, rodaq_storage_persistence_state state,
    const rodaq_status* error) const {
    if (!valid()) {
        return;
    }
    table_->persistence_completed(context_, operation, state, error);
}

void StorageService::report_failure(const rodaq_status* error) const {
    if (!valid()) {
        return;
    }
    table_->report_failure(context_, error);
}

StorageService::StorageService(void* context, const rodaq_runtime_storage* table) noexcept
: context_(context), table_(table) {}

Result<AcquisitionContext> AcquisitionContext::bind(ServiceRegistryRef services) {
    Result<InterfaceRef> common_interface = services.query(RODAQ_RUNTIME_COMMON_SERVICE_ID);
    if (!common_interface.ok()) {
        return std::move(common_interface).status();
    }
    auto common = CommonService::from(common_interface.value());
    if (!common.ok()) {
        return std::move(common).status();
    }
    Result<InterfaceRef> canonical_interface = services.query(RODAQ_RUNTIME_CANONICAL_SERVICE_ID);
    if (!canonical_interface.ok()) {
        return std::move(canonical_interface).status();
    }
    auto canonical = CanonicalService::from(canonical_interface.value());
    if (!canonical.ok()) {
        return std::move(canonical).status();
    }
    Result<InterfaceRef> projection_interface = services.query(RODAQ_RUNTIME_PROJECTION_SERVICE_ID);
    if (!projection_interface.ok()) {
        return std::move(projection_interface).status();
    }
    auto projection = ProjectionService::from(projection_interface.value());
    if (!projection.ok()) {
        return std::move(projection).status();
    }
    Result<InterfaceRef> acquisition_interface =
        services.query(RODAQ_RUNTIME_ACQUISITION_SERVICE_ID);
    if (!acquisition_interface.ok()) {
        return std::move(acquisition_interface).status();
    }
    auto acquisition = AcquisitionService::from(acquisition_interface.value());
    if (!acquisition.ok()) {
        return std::move(acquisition).status();
    }
    return AcquisitionContext{
        std::move(common.value()),
        std::move(canonical.value()),
        std::move(projection.value()),
        std::move(acquisition.value()),
    };
}

Result<StorageContext> StorageContext::bind(ServiceRegistryRef services) {
    Result<InterfaceRef> common_interface = services.query(RODAQ_RUNTIME_COMMON_SERVICE_ID);
    if (!common_interface.ok()) {
        return std::move(common_interface).status();
    }
    auto common = CommonService::from(common_interface.value());
    if (!common.ok()) {
        return std::move(common).status();
    }
    Result<InterfaceRef> canonical_interface = services.query(RODAQ_RUNTIME_CANONICAL_SERVICE_ID);
    if (!canonical_interface.ok()) {
        return std::move(canonical_interface).status();
    }
    auto canonical = CanonicalService::from(canonical_interface.value());
    if (!canonical.ok()) {
        return std::move(canonical).status();
    }
    Result<InterfaceRef> projection_interface = services.query(RODAQ_RUNTIME_PROJECTION_SERVICE_ID);
    if (!projection_interface.ok()) {
        return std::move(projection_interface).status();
    }
    auto projection = ProjectionService::from(projection_interface.value());
    if (!projection.ok()) {
        return std::move(projection).status();
    }
    Result<InterfaceRef> clock_interface =
        services.query(RODAQ_RUNTIME_CLOCK_CORRELATION_SERVICE_ID);
    if (!clock_interface.ok()) {
        return std::move(clock_interface).status();
    }
    auto clock = ClockCorrelationService::from(clock_interface.value());
    if (!clock.ok()) {
        return std::move(clock).status();
    }
    Result<InterfaceRef> storage_interface = services.query(RODAQ_RUNTIME_STORAGE_SERVICE_ID);
    if (!storage_interface.ok()) {
        return std::move(storage_interface).status();
    }
    auto storage = StorageService::from(storage_interface.value());
    if (!storage.ok()) {
        return std::move(storage).status();
    }
    return StorageContext{
        std::move(common.value()), std::move(canonical.value()), std::move(projection.value()),
        std::move(clock.value()),  std::move(storage.value()),
    };
}

#define RODAQ_SDK_CANONICAL_GETTER(method, handle_type, view_type)                         \
    Result<view_type> CanonicalService::method(handle_type handle) const {                 \
        if (!valid()) {                                                                    \
            return rodaq::Status(                                                          \
                StatusCode::InvalidState, "sdk", 0, "Canonical service is invalid");       \
        }                                                                                  \
        view_type value{};                                                                 \
        value.struct_size = sizeof(view_type);                                             \
        rodaq_status error{};                                                              \
        const rodaq_status_code status = table_->method(context_, handle, &value, &error); \
        if (status != RODAQ_STATUS_OK) {                                                   \
            return status_from_abi(status, &error, "sdk");                                 \
        }                                                                                  \
        return value;                                                                      \
    }

RODAQ_SDK_CANONICAL_GETTER(get_source, rodaq_source_handle, rodaq_source_info)
RODAQ_SDK_CANONICAL_GETTER(get_clock, rodaq_clock_handle, rodaq_clock_info)
RODAQ_SDK_CANONICAL_GETTER(get_schema, rodaq_schema_handle, rodaq_schema_info)
RODAQ_SDK_CANONICAL_GETTER(get_stream, rodaq_stream_handle, rodaq_stream_info)
RODAQ_SDK_CANONICAL_GETTER(
    get_stream_descriptor, rodaq_stream_descriptor_handle, rodaq_stream_descriptor_info)
#undef RODAQ_SDK_CANONICAL_GETTER

#define RODAQ_SDK_ACQ_REGISTER(method, abi_method, request_type, handle_type, validator)          \
    Result<handle_type> AcquisitionService::method(const request_type& request) const {           \
        if (!valid()) {                                                                           \
            return rodaq::Status(                                                                 \
                StatusCode::InvalidState, "sdk", 0, "Acquisition service is invalid");            \
        }                                                                                         \
        if (!plugin::validator(request)) {                                                        \
            return rodaq::Status(                                                                 \
                StatusCode::InvalidArgument, "sdk", 0,                                            \
                "Acquisition registration request is structurally invalid");                      \
        }                                                                                         \
        handle_type handle{};                                                                     \
        rodaq_status error{};                                                                     \
        const rodaq_status_code status = table_->abi_method(context_, &request, &handle, &error); \
        if (status != RODAQ_STATUS_OK) {                                                          \
            return status_from_abi(status, &error, "sdk");                                        \
        }                                                                                         \
        return handle;                                                                            \
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

}  // namespace rodaq::plugin
