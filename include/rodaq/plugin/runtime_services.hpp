#ifndef RODAQ_PLUGIN_RUNTIME_SERVICES_HPP_
#define RODAQ_PLUGIN_RUNTIME_SERVICES_HPP_

#include <rodaq/plugin/abi/runtime.h>
#include <rodaq/plugin/abi_support.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rodaq {

Result<void> runtime_result(rodaq_status_code status, const rodaq_status& error);

namespace plugin {

inline constexpr std::uint32_t kMaximumTransportTimestampKeys = 3;
inline constexpr std::uint32_t kMaximumCanonicalTimestampSlots = 4;

bool is_valid_source_registration(const rodaq_source_registration& request) noexcept;

bool is_valid_clock_registration(const rodaq_clock_registration& request) noexcept;

bool is_valid_schema_registration(const rodaq_schema_registration& request) noexcept;

bool is_valid_stream_registration(const rodaq_stream_registration& request) noexcept;

bool is_valid_stream_descriptor_registration(
    const rodaq_stream_descriptor_registration& request) noexcept;

bool is_valid_payload_input(const rodaq_payload_input& payload) noexcept;

bool is_valid_record_submission(const rodaq_record_submission& submission) noexcept;

}  // namespace plugin

class CommonRuntimeService final {
public:
    CommonRuntimeService() = default;
    bool valid() const noexcept;
    static Result<CommonRuntimeService> from(InterfaceView interface);

    void log(rodaq_log_level level, std::string_view category, std::string_view message) const;
    Result<std::uint64_t> monotonic_time_ns() const;
    Result<rodaq_runtime_lifecycle_snapshot> lifecycle_snapshot() const;
    void diagnostic(
        rodaq_diagnostic_severity severity, std::string_view code, std::string_view message) const;

private:
    CommonRuntimeService(void* context, const rodaq_runtime_common_v1* table) noexcept;
    void* context_ = nullptr;
    const rodaq_runtime_common_v1* table_ = nullptr;
};

class CanonicalRuntimeService final {
public:
    CanonicalRuntimeService() = default;
    bool valid() const noexcept;
    static Result<CanonicalRuntimeService> from(InterfaceView interface);

    Result<rodaq_source_view> get_source(rodaq_source_handle handle) const;
    Result<rodaq_clock_view> get_clock(rodaq_clock_handle handle) const;
    Result<rodaq_schema_view> get_schema(rodaq_schema_handle handle) const;
    Result<rodaq_stream_view> get_stream(rodaq_stream_handle handle) const;
    Result<rodaq_stream_descriptor_view> get_stream_descriptor(
        rodaq_stream_descriptor_handle handle) const;

    Result<rodaq_timestamp_key_set_view> get_timestamp_key_set(std::uint64_t id) const;

private:
    CanonicalRuntimeService(void* context, const rodaq_runtime_canonical_v1* table) noexcept;
    void* context_ = nullptr;
    const rodaq_runtime_canonical_v1* table_ = nullptr;
};

class ProjectionRuntimeService final {
public:
    ProjectionRuntimeService() = default;
    bool valid() const noexcept;
    static Result<ProjectionRuntimeService> from(InterfaceView interface);
    Result<void> register_entry(
        rodaq_stream_descriptor_handle descriptor, std::string_view id, const std::uint8_t* data,
        std::size_t size) const;
    Result<rodaq_projection_view> get(
        rodaq_stream_descriptor_handle descriptor, std::string_view id) const;

private:
    ProjectionRuntimeService(void* context, const rodaq_runtime_projection_v1* table) noexcept;
    void* context_ = nullptr;
    const rodaq_runtime_projection_v1* table_ = nullptr;
};

class ClockCorrelationRuntimeService final {
public:
    ClockCorrelationRuntimeService() = default;
    bool valid() const noexcept;
    static Result<ClockCorrelationRuntimeService> from(InterfaceView interface);
    Result<rodaq_unix_epoch_clock_anchor> anchor(rodaq_clock_handle clock) const;

private:
    ClockCorrelationRuntimeService(
        void* context, const rodaq_runtime_clock_correlation_v1* table) noexcept;
    void* context_ = nullptr;
    const rodaq_runtime_clock_correlation_v1* table_ = nullptr;
};

class AcquisitionRuntimeService final {
public:
    AcquisitionRuntimeService() = default;
    bool valid() const noexcept;
    static Result<AcquisitionRuntimeService> from(InterfaceView interface);

    Result<rodaq_source_handle> register_source(const rodaq_source_registration& request) const;
    Result<rodaq_clock_handle> register_clock(const rodaq_clock_registration& request) const;
    Result<rodaq_schema_handle> register_schema(const rodaq_schema_registration& request) const;
    Result<rodaq_stream_handle> register_stream(const rodaq_stream_registration& request) const;
    Result<rodaq_stream_descriptor_handle> publish_descriptor(
        const rodaq_stream_descriptor_registration& request) const;

    Result<rodaq_acquisition_submit_result> submit(const rodaq_record_submission& submission) const;
    Result<void> report_source(
        rodaq_source_handle source, rodaq_availability_state state,
        const rodaq_status* diagnostic) const;
    Result<void> report_stream(
        rodaq_stream_handle stream, rodaq_availability_state state,
        const rodaq_status* diagnostic) const;
    Result<void> report_plugin_state(
        rodaq_acquisition_plugin_state state, const rodaq_status* diagnostic) const;

private:
    AcquisitionRuntimeService(void* context, const rodaq_runtime_acquisition_v1* table) noexcept;
    void* context_ = nullptr;
    const rodaq_runtime_acquisition_v1* table_ = nullptr;
};

class StorageRuntimeService final {
public:
    StorageRuntimeService() = default;
    bool valid() const noexcept;
    static Result<StorageRuntimeService> from(InterfaceView interface);
    void payload_consumed(rodaq_storage_operation operation) const;
    void persistence_completed(
        rodaq_storage_operation operation, rodaq_storage_persistence_state state,
        const rodaq_status* error) const;
    void report_failure(const rodaq_status* error) const;

private:
    StorageRuntimeService(void* context, const rodaq_runtime_storage_v1* table) noexcept;
    void* context_ = nullptr;
    const rodaq_runtime_storage_v1* table_ = nullptr;
};

struct AcquisitionRuntime {
    CommonRuntimeService common;
    CanonicalRuntimeService canonical;
    ProjectionRuntimeService projection;
    AcquisitionRuntimeService acquisition;

    static Result<AcquisitionRuntime> bind(ServiceRegistryView services);
};

struct StorageRuntime {
    CommonRuntimeService common;
    CanonicalRuntimeService canonical;
    ProjectionRuntimeService projection;
    ClockCorrelationRuntimeService clock_correlation;
    StorageRuntimeService storage;

    static Result<StorageRuntime> bind(ServiceRegistryView services);
};

}  // namespace rodaq

#endif  // RODAQ_PLUGIN_RUNTIME_SERVICES_HPP_
