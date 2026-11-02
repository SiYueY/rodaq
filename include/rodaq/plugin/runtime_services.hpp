#ifndef RODAQ_PLUGIN_RUNTIME_SERVICES_HPP_
#define RODAQ_PLUGIN_RUNTIME_SERVICES_HPP_

#include <rodaq/plugin/abi/runtime.h>
#include <rodaq/plugin/abi_support.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <cstring>
#include <string_view>
#include <utility>

namespace rodaq {

template <typename Table>
Result<Table> runtime_table(InterfaceView interface) {
    if (!interface.Valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "sdk", 0, "Runtime interface is incomplete"));
    }
    const auto* table = static_cast<const Table*>(interface.VTable());
    if (table->protocol_version != Table{}.protocol_version) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "sdk", 0, "Runtime protocol is incompatible"));
    }
    return table;
}

inline Result<void> runtime_result(rodaq_status status, const rodaq_error& error) {
    if (status == RODAQ_STATUS_OK) {
        return {};
    }
    return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error());
}

class CommonRuntimeService final {
public:
    CommonRuntimeService() = default;
    static Result<CommonRuntimeService> From(InterfaceView interface) {
        if (!interface.Valid()) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidArgument, "sdk", 0, "Common Runtime interface is incomplete"));
        }
        const auto* table = static_cast<const rodaq_runtime_common_v1*>(interface.VTable());
        if (table->protocol_version != RODAQ_RUNTIME_COMMON_PROTOCOL_VERSION ||
            table->struct_size < RODAQ_RUNTIME_COMMON_V1_MIN_STRUCT_SIZE || table->log == nullptr ||
            table->monotonic_time_ns == nullptr || table->get_lifecycle_snapshot == nullptr ||
            table->report_diagnostic == nullptr) {
            return nonstd::make_unexpected(
                Error(ErrorCode::InvalidArgument, "sdk", 0, "Common Runtime ABI is incompatible"));
        }
        return CommonRuntimeService(interface.Context(), table);
    }

    void Log(rodaq_log_level level, std::string_view category, std::string_view message) const {
        table_->log(
            context_, level, rodaq_string_view{category.data(), category.size()},
            rodaq_string_view{message.data(), message.size()});
    }
    std::uint64_t MonotonicTimeNs() const { return table_->monotonic_time_ns(context_); }
    Result<rodaq_runtime_lifecycle_snapshot> LifecycleSnapshot() const {
        rodaq_runtime_lifecycle_snapshot value{};
        rodaq_error error{};
        const rodaq_status status = table_->get_lifecycle_snapshot(context_, &value, &error);
        if (status != RODAQ_STATUS_OK) {
            return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error());
        }
        return value;
    }
    void Diagnostic(
        rodaq_diagnostic_severity severity, std::string_view code, std::string_view message) const {
        table_->report_diagnostic(
            context_, severity, rodaq_string_view{code.data(), code.size()},
            rodaq_string_view{message.data(), message.size()});
    }

private:
    CommonRuntimeService(void* context, const rodaq_runtime_common_v1* table) noexcept
    : context_(context), table_(table) {}
    void* context_ = nullptr;
    const rodaq_runtime_common_v1* table_ = nullptr;
};

class CanonicalRuntimeService final {
public:
    CanonicalRuntimeService() = default;
    static Result<CanonicalRuntimeService> From(InterfaceView interface) {
        if (!interface.Valid()) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidArgument, "sdk", 0, "Canonical Runtime interface is incomplete"));
        }
        const auto* table = static_cast<const rodaq_runtime_canonical_v1*>(interface.VTable());
        if (table->protocol_version != RODAQ_RUNTIME_CANONICAL_PROTOCOL_VERSION ||
            table->struct_size < RODAQ_RUNTIME_CANONICAL_V1_MIN_STRUCT_SIZE ||
            table->get_source == nullptr || table->get_clock == nullptr ||
            table->get_schema == nullptr || table->get_timestamp_key_set == nullptr ||
            table->get_stream == nullptr || table->get_stream_descriptor == nullptr) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidArgument, "sdk", 0, "Canonical Runtime ABI is incompatible"));
        }
        return CanonicalRuntimeService(interface.Context(), table);
    }

#define RODAQ_SDK_CANONICAL_GETTER(method, handle_type, view_type)                             \
    Result<view_type> method(handle_type handle) const {                                       \
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

    Result<rodaq_timestamp_key_set_view> GetTimestampKeySet(std::uint64_t id) const {
        rodaq_timestamp_key_set_view value{};
        value.struct_size = sizeof(value);
        rodaq_error error{};
        const rodaq_status status = table_->get_timestamp_key_set(context_, id, &value, &error);
        if (status != RODAQ_STATUS_OK) {
            return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error());
        }
        return value;
    }

private:
    CanonicalRuntimeService(void* context, const rodaq_runtime_canonical_v1* table) noexcept
    : context_(context), table_(table) {}
    void* context_ = nullptr;
    const rodaq_runtime_canonical_v1* table_ = nullptr;
};

class ProjectionRuntimeService final {
public:
    ProjectionRuntimeService() = default;
    static Result<ProjectionRuntimeService> From(InterfaceView interface) {
        if (!interface.Valid()) {
            return nonstd::make_unexpected(
                Error(ErrorCode::InvalidArgument, "sdk", 0, "Projection interface is incomplete"));
        }
        const auto* table = static_cast<const rodaq_runtime_projection_v1*>(interface.VTable());
        if (table->protocol_version != RODAQ_RUNTIME_PROJECTION_PROTOCOL_VERSION ||
            table->struct_size < RODAQ_RUNTIME_PROJECTION_V1_MIN_STRUCT_SIZE ||
            table->register_projection == nullptr || table->get_projection == nullptr) {
            return nonstd::make_unexpected(
                Error(ErrorCode::InvalidArgument, "sdk", 0, "Projection ABI is incompatible"));
        }
        return ProjectionRuntimeService(interface.Context(), table);
    }
    Result<void> Register(
        rodaq_stream_descriptor_handle descriptor, std::string_view id, const std::uint8_t* data,
        std::size_t size) const {
        rodaq_projection_registration request{};
        request.struct_size = sizeof(request);
        request.descriptor = descriptor;
        request.projection_id = rodaq_string_view{id.data(), id.size()};
        request.payload = rodaq_bytes_view{data, size};
        rodaq_error error{};
        return runtime_result(table_->register_projection(context_, &request, &error), error);
    }
    Result<rodaq_projection_view> Get(
        rodaq_stream_descriptor_handle descriptor, std::string_view id) const {
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

private:
    ProjectionRuntimeService(void* context, const rodaq_runtime_projection_v1* table) noexcept
    : context_(context), table_(table) {}
    void* context_ = nullptr;
    const rodaq_runtime_projection_v1* table_ = nullptr;
};

class ClockCorrelationRuntimeService final {
public:
    ClockCorrelationRuntimeService() = default;
    static Result<ClockCorrelationRuntimeService> From(InterfaceView interface) {
        if (!interface.Valid()) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidArgument, "sdk", 0, "Clock-correlation interface is incomplete"));
        }
        const auto* table =
            static_cast<const rodaq_runtime_clock_correlation_v1*>(interface.VTable());
        if (table->protocol_version != RODAQ_RUNTIME_CLOCK_CORRELATION_PROTOCOL_VERSION ||
            table->struct_size < RODAQ_RUNTIME_CLOCK_CORRELATION_V1_MIN_STRUCT_SIZE ||
            table->get_unix_epoch_anchor == nullptr) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidArgument, "sdk", 0, "Clock-correlation ABI is incompatible"));
        }
        return ClockCorrelationRuntimeService(interface.Context(), table);
    }
    Result<rodaq_unix_epoch_clock_anchor> Anchor(rodaq_clock_handle clock) const {
        rodaq_unix_epoch_clock_anchor value{};
        value.struct_size = sizeof(value);
        rodaq_error error{};
        const rodaq_status status = table_->get_unix_epoch_anchor(context_, clock, &value, &error);
        if (status != RODAQ_STATUS_OK) {
            return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error());
        }
        return value;
    }

private:
    ClockCorrelationRuntimeService(
        void* context, const rodaq_runtime_clock_correlation_v1* table) noexcept
    : context_(context), table_(table) {}
    void* context_ = nullptr;
    const rodaq_runtime_clock_correlation_v1* table_ = nullptr;
};

class AcquisitionRuntimeService final {
public:
    AcquisitionRuntimeService() = default;
    static Result<AcquisitionRuntimeService> From(InterfaceView interface) {
        if (!interface.Valid()) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidArgument, "sdk", 0,
                "Acquisition Runtime interface is incomplete"));
        }
        const auto* table = static_cast<const rodaq_runtime_acquisition_v1*>(interface.VTable());
        if (table->protocol_version != RODAQ_RUNTIME_ACQUISITION_PROTOCOL_VERSION ||
            table->struct_size < RODAQ_RUNTIME_ACQUISITION_V1_MIN_STRUCT_SIZE ||
            table->register_source == nullptr || table->register_clock == nullptr ||
            table->register_schema == nullptr || table->register_stream == nullptr ||
            table->publish_stream_descriptor == nullptr ||
            table->report_source_availability == nullptr ||
            table->report_stream_availability == nullptr || table->report_plugin_state == nullptr ||
            table->submit == nullptr) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidArgument, "sdk", 0, "Acquisition Runtime ABI is incompatible"));
        }
        return AcquisitionRuntimeService(interface.Context(), table);
    }

#define RODAQ_SDK_ACQ_REGISTER(method, abi_method, request_type, handle_type)                  \
    Result<handle_type> method(const request_type& request) const {                            \
        handle_type handle{};                                                                  \
        rodaq_error error{};                                                                   \
        const rodaq_status status = table_->abi_method(context_, &request, &handle, &error);   \
        if (status != RODAQ_STATUS_OK) {                                                       \
            return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error()); \
        }                                                                                      \
        return handle;                                                                         \
    }
    RODAQ_SDK_ACQ_REGISTER(
        RegisterSource, register_source, rodaq_source_registration, rodaq_source_handle)
    RODAQ_SDK_ACQ_REGISTER(
        RegisterClock, register_clock, rodaq_clock_registration, rodaq_clock_handle)
    RODAQ_SDK_ACQ_REGISTER(
        RegisterSchema, register_schema, rodaq_schema_registration, rodaq_schema_handle)
    RODAQ_SDK_ACQ_REGISTER(
        RegisterStream, register_stream, rodaq_stream_registration, rodaq_stream_handle)
    RODAQ_SDK_ACQ_REGISTER(
        PublishDescriptor, publish_stream_descriptor, rodaq_stream_descriptor_registration,
        rodaq_stream_descriptor_handle)
#undef RODAQ_SDK_ACQ_REGISTER

    Result<rodaq_acquisition_submit_result> Submit(
        const rodaq_record_submission& submission) const {
        rodaq_acquisition_submit_result result{};
        result.struct_size = sizeof(result);
        rodaq_error error{};
        const rodaq_status status = table_->submit(context_, &submission, &result, &error);
        if (status != RODAQ_STATUS_OK) {
            return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error());
        }
        return result;
    }
    Result<void> ReportSource(
        rodaq_source_handle source, rodaq_availability_state state,
        const rodaq_error* diagnostic) const {
        rodaq_error error{};
        return runtime_result(
            table_->report_source_availability(context_, source, state, diagnostic, &error), error);
    }
    Result<void> ReportStream(
        rodaq_stream_handle stream, rodaq_availability_state state,
        const rodaq_error* diagnostic) const {
        rodaq_error error{};
        return runtime_result(
            table_->report_stream_availability(context_, stream, state, diagnostic, &error), error);
    }
    Result<void> ReportPluginState(
        rodaq_acquisition_plugin_state state, const rodaq_error* diagnostic) const {
        rodaq_error error{};
        return runtime_result(
            table_->report_plugin_state(context_, state, diagnostic, &error), error);
    }

private:
    AcquisitionRuntimeService(void* context, const rodaq_runtime_acquisition_v1* table) noexcept
    : context_(context), table_(table) {}
    void* context_ = nullptr;
    const rodaq_runtime_acquisition_v1* table_ = nullptr;
};

class StorageRuntimeService final {
public:
    StorageRuntimeService() = default;
    static Result<StorageRuntimeService> From(InterfaceView interface) {
        if (!interface.Valid()) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidArgument, "sdk", 0, "Storage Runtime interface is incomplete"));
        }
        const auto* table = static_cast<const rodaq_runtime_storage_v1*>(interface.VTable());
        if (table->protocol_version != RODAQ_RUNTIME_STORAGE_PROTOCOL_VERSION ||
            table->struct_size < RODAQ_RUNTIME_STORAGE_V1_MIN_STRUCT_SIZE ||
            table->payload_consumed == nullptr || table->persistence_completed == nullptr ||
            table->report_failure == nullptr) {
            return nonstd::make_unexpected(
                Error(ErrorCode::InvalidArgument, "sdk", 0, "Storage Runtime ABI is incompatible"));
        }
        return StorageRuntimeService(interface.Context(), table);
    }
    void PayloadConsumed(rodaq_storage_operation operation) const {
        table_->payload_consumed(context_, operation);
    }
    void PersistenceCompleted(
        rodaq_storage_operation operation, rodaq_storage_persistence_state state,
        const rodaq_error* error) const {
        table_->persistence_completed(context_, operation, state, error);
    }
    void ReportFailure(const rodaq_error* error) const { table_->report_failure(context_, error); }

private:
    StorageRuntimeService(void* context, const rodaq_runtime_storage_v1* table) noexcept
    : context_(context), table_(table) {}
    void* context_ = nullptr;
    const rodaq_runtime_storage_v1* table_ = nullptr;
};

struct AcquisitionRuntime {
    CommonRuntimeService common;
    CanonicalRuntimeService canonical;
    ProjectionRuntimeService projection;
    AcquisitionRuntimeService acquisition;

    static Result<AcquisitionRuntime> Bind(ServiceRegistryView services) {
        Result<InterfaceView> common_interface = services.Query(RODAQ_RUNTIME_COMMON_SERVICE_ID);
        if (!common_interface.has_value()) {
            return nonstd::make_unexpected(std::move(common_interface.error()));
        }
        auto common = CommonRuntimeService::From(common_interface.value());
        if (!common.has_value()) {
            return nonstd::make_unexpected(std::move(common.error()));
        }
        Result<InterfaceView> canonical_interface =
            services.Query(RODAQ_RUNTIME_CANONICAL_SERVICE_ID);
        if (!canonical_interface.has_value()) {
            return nonstd::make_unexpected(std::move(canonical_interface.error()));
        }
        auto canonical = CanonicalRuntimeService::From(canonical_interface.value());
        if (!canonical.has_value()) {
            return nonstd::make_unexpected(std::move(canonical.error()));
        }
        Result<InterfaceView> projection_interface =
            services.Query(RODAQ_RUNTIME_PROJECTION_SERVICE_ID);
        if (!projection_interface.has_value()) {
            return nonstd::make_unexpected(std::move(projection_interface.error()));
        }
        auto projection = ProjectionRuntimeService::From(projection_interface.value());
        if (!projection.has_value()) {
            return nonstd::make_unexpected(std::move(projection.error()));
        }
        Result<InterfaceView> acquisition_interface =
            services.Query(RODAQ_RUNTIME_ACQUISITION_SERVICE_ID);
        if (!acquisition_interface.has_value()) {
            return nonstd::make_unexpected(std::move(acquisition_interface.error()));
        }
        auto acquisition = AcquisitionRuntimeService::From(acquisition_interface.value());
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
};

struct StorageRuntime {
    CommonRuntimeService common;
    CanonicalRuntimeService canonical;
    ProjectionRuntimeService projection;
    ClockCorrelationRuntimeService clock_correlation;
    StorageRuntimeService storage;

    static Result<StorageRuntime> Bind(ServiceRegistryView services) {
        Result<InterfaceView> common_interface = services.Query(RODAQ_RUNTIME_COMMON_SERVICE_ID);
        if (!common_interface.has_value()) {
            return nonstd::make_unexpected(std::move(common_interface.error()));
        }
        auto common = CommonRuntimeService::From(common_interface.value());
        if (!common.has_value()) {
            return nonstd::make_unexpected(std::move(common.error()));
        }
        Result<InterfaceView> canonical_interface =
            services.Query(RODAQ_RUNTIME_CANONICAL_SERVICE_ID);
        if (!canonical_interface.has_value()) {
            return nonstd::make_unexpected(std::move(canonical_interface.error()));
        }
        auto canonical = CanonicalRuntimeService::From(canonical_interface.value());
        if (!canonical.has_value()) {
            return nonstd::make_unexpected(std::move(canonical.error()));
        }
        Result<InterfaceView> projection_interface =
            services.Query(RODAQ_RUNTIME_PROJECTION_SERVICE_ID);
        if (!projection_interface.has_value()) {
            return nonstd::make_unexpected(std::move(projection_interface.error()));
        }
        auto projection = ProjectionRuntimeService::From(projection_interface.value());
        if (!projection.has_value()) {
            return nonstd::make_unexpected(std::move(projection.error()));
        }
        Result<InterfaceView> clock_interface =
            services.Query(RODAQ_RUNTIME_CLOCK_CORRELATION_SERVICE_ID);
        if (!clock_interface.has_value()) {
            return nonstd::make_unexpected(std::move(clock_interface.error()));
        }
        auto clock = ClockCorrelationRuntimeService::From(clock_interface.value());
        if (!clock.has_value()) {
            return nonstd::make_unexpected(std::move(clock.error()));
        }
        Result<InterfaceView> storage_interface = services.Query(RODAQ_RUNTIME_STORAGE_SERVICE_ID);
        if (!storage_interface.has_value()) {
            return nonstd::make_unexpected(std::move(storage_interface.error()));
        }
        auto storage = StorageRuntimeService::From(storage_interface.value());
        if (!storage.has_value()) {
            return nonstd::make_unexpected(std::move(storage.error()));
        }
        return StorageRuntime{
            std::move(common.value()), std::move(canonical.value()), std::move(projection.value()),
            std::move(clock.value()),  std::move(storage.value()),
        };
    }
};

}  // namespace rodaq

#endif  // RODAQ_PLUGIN_RUNTIME_SERVICES_HPP_
