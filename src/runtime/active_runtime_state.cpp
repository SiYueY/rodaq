#include "runtime/active_runtime_state.hpp"

#include "plugin/plugin_status.hpp"
#include "runtime/quarantine.hpp"
#include "runtime/spdlog_backend.hpp"

#include <rodaq/plugin/abi_support.hpp>
#include <rodaq/plugin/runtime_services.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rodaq::runtime {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view kIngressTimestampRole = "rodaq/acquisition_ingress";
constexpr std::size_t kMaxSecondaryFailures = 8;
constexpr std::size_t kRuntimeClockPluginIndex = std::numeric_limits<std::size_t>::max();

Status runtime_status(StatusCode status_code, std::string message) {
    return Status(status_code, "runtime", 0, std::move(message));
}

std::unique_ptr<LatencyHistogram> make_latency_histogram() {
    auto result = LatencyHistogram::create(1, 60LL * 1'000'000'000LL, 3);
    if (!result.ok()) {
        throw std::runtime_error(std::string(result.status().message()));
    }
    return std::move(result.value());
}

LatencySnapshot latency_snapshot(const LatencyHistogram& histogram) noexcept {
    LatencySnapshot snapshot;
    snapshot.count = histogram.count();
    if (snapshot.count == 0) {
        return snapshot;
    }
    snapshot.minimum_ns = histogram.min();
    snapshot.maximum_ns = histogram.max();
    snapshot.p50_ns = histogram.p50();
    snapshot.p95_ns = histogram.p95();
    snapshot.p99_ns = histogram.p99();
    snapshot.p999_ns = histogram.percentile(99.9);
    return snapshot;
}

template <typename T, typename HandleType>
Registry<T, HandleType> make_registry(std::size_t capacity, std::uint32_t generation) {
    auto result = Registry<T, HandleType>::create(capacity, generation);
    if (!result.ok()) {
        throw std::runtime_error(std::string(result.status().message()));
    }
    return std::move(result.value());
}

std::uint32_t registry_generation(std::uint64_t activation_generation) noexcept {
    const auto generation = static_cast<std::uint32_t>(activation_generation);
    return generation == 0 ? 1U : generation;
}

ArenaBytes copy_bytes(
    std::pmr::memory_resource* resource, const std::uint8_t* data, std::size_t size) {
    ArenaBytes result(resource);
    if (size != 0) {
        result.assign(data, data + size);
    }
    return result;
}

ArenaString copy_string(std::pmr::memory_resource* resource, std::string_view value) {
    ArenaString result(resource);
    result.assign(value.data(), value.size());
    return result;
}

Result<std::int64_t> capture_ingress_reading() {
    const auto count =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count();
    if (count > std::numeric_limits<std::int64_t>::max()) {
        return runtime_status(
            StatusCode::InternalError, "Runtime ingress clock reading overflowed");
    }
    return static_cast<std::int64_t>(count);
}

bool bytes_equal(const ArenaBytes& stored, const std::uint8_t* data, std::size_t size) noexcept {
    return stored.size() == size && std::equal(stored.begin(), stored.end(), data);
}

bool strings_equal(const ArenaString& stored, std::string_view value) noexcept {
    return stored.size() == value.size() && std::equal(stored.begin(), stored.end(), value.begin());
}

template <typename Struct>
Result<void> write_output_prefix(const Struct& output, Struct* destination) {
    if (destination == nullptr) {
        return runtime_status(StatusCode::InvalidArgument, "Runtime output pointer is null");
    }
    const std::uint32_t capacity = destination->struct_size;
    const std::uint32_t initialized = initialized_prefix_size<Struct>(capacity);
    std::memset(destination, 0, initialized);
    std::memcpy(destination, &output, initialized);
    destination->struct_size = initialized;
    return {};
}

Status status_from_abi_diagnostic(const rodaq_status& error) {
    const std::string_view domain(
        error.domain, std::min<std::size_t>(error.domain_size, sizeof(error.domain)));
    const std::string_view message(
        error.message, std::min<std::size_t>(error.message_size, sizeof(error.message)));
    StatusCode status_code = StatusCode::InternalError;
    switch (error.status_code) {
        case RODAQ_STATUS_INVALID_ARGUMENT:
            status_code = StatusCode::InvalidArgument;
            break;
        case RODAQ_STATUS_INVALID_STATE:
            status_code = StatusCode::InvalidState;
            break;
        case RODAQ_STATUS_NOT_FOUND:
            status_code = StatusCode::NotFound;
            break;
        case RODAQ_STATUS_CONFLICT:
            status_code = StatusCode::Conflict;
            break;
        case RODAQ_STATUS_RESOURCE_EXHAUSTED:
            status_code = StatusCode::ResourceExhausted;
            break;
        case RODAQ_STATUS_UNAVAILABLE:
            status_code = StatusCode::Unavailable;
            break;
        case RODAQ_STATUS_STORAGE_ERROR:
            status_code = StatusCode::StorageError;
            break;
        case RODAQ_STATUS_TIMEOUT:
            status_code = StatusCode::Timeout;
            break;
        case RODAQ_STATUS_UNKNOWN_ERROR:
            status_code = StatusCode::UnknownError;
            break;
        default:
            status_code = StatusCode::InternalError;
            break;
    }
    return Status(status_code, std::string(domain), error.error_code, std::string(message));
}

rodaq_status_code assign_status(const Status& error, rodaq_status* out_status) noexcept {
    assign_abi_status(error, out_status);
    return abi_status_from_status_code(error.status_code());
}

rodaq_status_code assign_message(
    StatusCode status_code, std::string_view message, rodaq_status* out_status) noexcept {
    assign_abi_status(abi_status_from_status_code(status_code), "runtime", 0, message, out_status);
    return abi_status_from_status_code(status_code);
}

rodaq_acquisition_submit_result submit_result(
    rodaq_admission_outcome outcome, rodaq_admission_rejection_reason reason,
    rodaq_payload_disposition disposition) noexcept {
    return rodaq_acquisition_submit_result{
        sizeof(rodaq_acquisition_submit_result), outcome, reason, disposition, {0, 0},
    };
}

ActiveRuntimeState* state_from(void* context) noexcept {
    return static_cast<ActiveRuntimeState*>(context);
}

const AcquisitionServiceContext* acquisition_context_from(void* context) noexcept {
    return static_cast<const AcquisitionServiceContext*>(context);
}

const StorageServiceContext* storage_context_from(void* context) noexcept {
    return static_cast<const StorageServiceContext*>(context);
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
        state->log(level, parsed_category, parsed_message);
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
        *out_snapshot = result.value();
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
    return canonical_view(context, out_view, out_status, [handle](ActiveRuntimeState& state) {
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
    return canonical_view(context, out_view, out_status, [handle](ActiveRuntimeState& state) {
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
    return canonical_view(context, out_view, out_status, [handle](ActiveRuntimeState& state) {
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
    return canonical_view(context, out_view, out_status, [id](ActiveRuntimeState& state) {
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
    return canonical_view(context, out_view, out_status, [handle](ActiveRuntimeState& state) {
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
    return canonical_view(context, out_view, out_status, [handle](ActiveRuntimeState& state) {
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
            state->request_failure(runtime_status(
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
                ->request_failure(runtime_status(
                    StatusCode::InvalidArgument,
                    "Storage failure callback supplied a malformed diagnostic"));
            return;
        }
        Status failure = error == nullptr
                             ? runtime_status(StatusCode::StorageError, "Storage reported failure")
                             : status_from_abi_diagnostic(*error);
        state_from(storage_context_from(context)->state)->request_failure(std::move(failure));
    } catch (...) {
    }
}

}  // namespace

const rodaq_runtime_common_v1 ActiveRuntimeState::kCommonVtable = {
    RODAQ_RUNTIME_COMMON_PROTOCOL_VERSION,
    RODAQ_RUNTIME_COMMON_V1_MIN_STRUCT_SIZE,
    &common_log,
    &common_monotonic_time_ns,
    &common_lifecycle_snapshot,
    &common_diagnostic,
};

const rodaq_runtime_canonical_v1 ActiveRuntimeState::kCanonicalVtable = {
    RODAQ_RUNTIME_CANONICAL_PROTOCOL_VERSION,
    RODAQ_RUNTIME_CANONICAL_V1_MIN_STRUCT_SIZE,
    &canonical_get_source,
    &canonical_get_clock,
    &canonical_get_schema,
    &canonical_get_timestamp_key_set,
    &canonical_get_stream,
    &canonical_get_stream_descriptor,
};

const rodaq_runtime_acquisition_v1 ActiveRuntimeState::kAcquisitionVtable = {
    RODAQ_RUNTIME_ACQUISITION_PROTOCOL_VERSION,
    RODAQ_RUNTIME_ACQUISITION_V1_MIN_STRUCT_SIZE,
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

const rodaq_runtime_storage_v1 ActiveRuntimeState::kStorageVtable = {
    RODAQ_RUNTIME_STORAGE_PROTOCOL_VERSION,
    RODAQ_RUNTIME_STORAGE_V1_MIN_STRUCT_SIZE,
    &storage_payload_consumed,
    &storage_persistence_completed,
    &storage_report_failure,
};

Result<std::unique_ptr<ActiveRuntimeState>> ActiveRuntimeState::create(
    RuntimeConfig config, std::uint64_t activation_generation) {
    try {
        return std::unique_ptr<ActiveRuntimeState>(
            new ActiveRuntimeState(std::move(config), activation_generation));
    } catch (const std::bad_alloc&) {
        return runtime_status(
            StatusCode::ResourceExhausted, "Runtime activation allocation failed");
    } catch (const std::exception& error) {
        return runtime_status(StatusCode::InternalError, error.what());
    } catch (...) {
        return runtime_status(StatusCode::InternalError, "Runtime activation construction failed");
    }
}

ActiveRuntimeState::ActiveRuntimeState(RuntimeConfig config, std::uint64_t activation_generation)
: options_(std::move(config)),
  activation_generation_(activation_generation),
  arena_([this] {
      auto result = StructuralArena::create(options_.resources.structural_arena_bytes);
      if (!result.ok()) {
          throw std::runtime_error(std::string(result.status().message()));
      }
      return std::move(result.value());
  }()),
  sources_(make_registry<SourceEntry, SourceHandle>(
      options_.resources.max_sources, registry_generation(activation_generation))),
  clocks_(make_registry<ClockEntry, ClockHandle>(
      options_.resources.max_clocks, registry_generation(activation_generation))),
  schemas_(make_registry<SchemaEntry, SchemaHandle>(
      options_.resources.max_schemas, registry_generation(activation_generation))),
  timestamp_key_sets_(make_registry<TimestampKeySetEntry, TimestampKeySetHandle>(
      options_.resources.max_timestamp_key_sets, registry_generation(activation_generation))),
  streams_(make_registry<StreamEntry, StreamHandle>(
      options_.resources.max_streams, registry_generation(activation_generation))),
  descriptors_(make_registry<StreamDescriptorEntry, StreamDescriptorHandle>(
      options_.resources.max_stream_descriptors, registry_generation(activation_generation))),
  bindings_(make_registry<BindingEntry, BindingHandle>(
      options_.resources.max_bindings, registry_generation(activation_generation))),
  sequences_([this] {
      auto result = AcceptanceSequenceAllocator::create(options_.resources.max_streams);
      if (!result.ok()) {
          throw std::runtime_error(std::string(result.status().message()));
      }
      return std::move(result.value());
  }()),
  payload_pool_([this] {
      auto result = PayloadPool::create(options_.resources.payload_pool);
      if (!result.ok()) {
          throw std::runtime_error(std::string(result.status().message()));
      }
      return std::move(result.value());
  }()),
  projections_(ProjectionLimits{128, 8, 1024U * 1024U, 8U * 1024U * 1024U}),
  state_(RuntimeState::Starting),
  admission_wait_latency_(make_latency_histogram()),
  storage_handoff_latency_(make_latency_histogram()),
  payload_consumed_latency_(make_latency_histogram()),
  persistence_latency_(make_latency_histogram()),
  stop_latency_(make_latency_histogram()) {
    source_handles_.reserve(options_.resources.max_sources);
    clock_handles_.reserve(options_.resources.max_clocks);
    schema_handles_.reserve(options_.resources.max_schemas);
    timestamp_key_set_handles_.reserve(options_.resources.max_timestamp_key_sets);
    stream_handles_.reserve(options_.resources.max_streams);
    descriptor_handles_.reserve(options_.resources.max_stream_descriptors);
    binding_handles_.reserve(options_.resources.max_bindings);
    secondary_failures_.reserve(kMaxSecondaryFailures);
    descriptor_worker_requests_.resize(options_.resources.max_stream_descriptors);
    acquisition_controls_.resize(options_.acquisitions.size());
    acquisition_instances_.resize(options_.acquisitions.size());
    acquisition_health_.reserve(options_.acquisitions.size());
    for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
        acquisition_health_.push_back(
            std::make_unique<std::atomic<std::uint32_t>>(RODAQ_ACQUISITION_PLUGIN_AVAILABLE));
    }
    acquisition_contexts_.reserve(options_.acquisitions.size());
    for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
        auto context = std::make_unique<AcquisitionServiceContext>();
        context->state = this;
        context->plugin_index = index;
        acquisition_contexts_.push_back(std::move(context));
    }
    storage_context_ = std::make_unique<StorageServiceContext>();
    storage_context_->state = this;
    auto logging = LoggingContext::create(options_.logging, std::make_shared<SpdlogBackend>());
    if (!logging.ok()) {
        throw std::runtime_error(std::string(logging.status().message()));
    }
    logging_context_ = std::move(logging.value());
    auto ingress = initialize_ingress_clock();
    if (!ingress.ok()) {
        throw std::runtime_error(std::string(ingress.status().message()));
    }
    projections_.set_registration_sink(
        [this](rodaq_stream_descriptor_handle descriptor, std::string_view projection_id) {
            on_projection_registered(descriptor, projection_id);
        });
}

ActiveRuntimeState::~ActiveRuntimeState() noexcept {
    payload_pool_.set_capacity_changed_sink({});
    if (!descriptor_worker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
        descriptor_worker_closing_ = true;
        descriptor_worker_count_ = 0;
        descriptor_worker_condition_.notify_all();
    }
    descriptor_worker_.join();
}

void ActiveRuntimeState::set_acquisition_control(
    std::size_t plugin_index, rodaq_interface control) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (plugin_index >= acquisition_controls_.size() || control.context == nullptr ||
        control.vtable == nullptr) {
        throw std::invalid_argument("Acquisition control interface is invalid");
    }
    acquisition_controls_[plugin_index] = control;
}

void ActiveRuntimeState::set_storage_control(rodaq_interface control) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (control.context == nullptr || control.vtable == nullptr) {
        throw std::invalid_argument("Storage control interface is invalid");
    }
    storage_control_ = control;
}

Result<void> ActiveRuntimeState::initialize_ingress_clock() {
    static constexpr std::string_view kNativeId = "rodaq/runtime/ingress";
    static constexpr std::string_view kOrigin = "monotonic_since_boot";

    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (ingress_clock_.valid()) {
            return runtime_status(
                StatusCode::InvalidState, "Runtime ingress Clock is already created");
        }
        auto inserted = clocks_.insert(ClockEntry{
            copy_bytes(
                arena_->resource(), reinterpret_cast<const std::uint8_t*>(kNativeId.data()),
                kNativeId.size()),
            next_clock_id_++,
            1,
            1'000'000'000,
            copy_string(arena_->resource(), kOrigin),
            kRuntimeClockPluginIndex,
        });
        if (!inserted.ok()) {
            return std::move(inserted).status();
        }
        ingress_clock_ = inserted.value();
        clock_handles_.push_back(ingress_clock_);
    }

    auto correlation = ClockCorrelation::establish(
        rodaq_clock_handle{ingress_clock_.value()},
        []() -> std::int64_t {
            auto reading = capture_ingress_reading();
            if (!reading.ok()) {
                throw std::runtime_error(std::string(reading.status().message()));
            }
            return reading.value();
        },
        []() -> std::uint64_t {
            const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
            if (count < 0) {
                throw std::runtime_error("System clock is before Unix epoch");
            }
            return static_cast<std::uint64_t>(count);
        });
    if (!correlation.ok()) {
        return std::move(correlation).status();
    }
    ingress_clock_correlation_.emplace(std::move(correlation.value()));
    return {};
}

Result<rodaq_storage_capabilities> ActiveRuntimeState::storage_capabilities() const {
    if (!storage_control_.has_value()) {
        return runtime_status(StatusCode::InvalidState, "Storage control interface is unavailable");
    }
    const auto* table = static_cast<const rodaq_storage_control_v1*>(storage_control_->vtable);
    if (table->get_capabilities == nullptr) {
        return runtime_status(StatusCode::InvalidArgument, "Storage capabilities slot is missing");
    }
    rodaq_storage_capabilities capabilities{};
    capabilities.struct_size = sizeof(capabilities);
    rodaq_status error{};
    const rodaq_status_code status =
        table->get_capabilities(storage_control_->context, &capabilities, &error);
    if (status != RODAQ_STATUS_OK) {
        auto converted = status_from_abi(status, &error, "storage");
        return converted.ok()
                   ? runtime_status(StatusCode::InternalError, "Storage capabilities failed")
                   : std::move(converted);
    }
    if (capabilities.struct_size < RODAQ_STORAGE_CAPABILITIES_MIN_STRUCT_SIZE) {
        return runtime_status(
            StatusCode::InvalidArgument, "Storage capabilities output is incomplete");
    }
    return capabilities;
}

void ActiveRuntimeState::set_descriptor_progress_timeout(std::chrono::nanoseconds timeout) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    descriptor_progress_timeout_ = timeout;
}

void ActiveRuntimeState::set_binding_progress_timeout(std::chrono::nanoseconds timeout) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    binding_progress_timeout_ = timeout;
}

std::shared_ptr<void> ActiveRuntimeState::track_control_call() {
    auto state = shared_from_this();
    // shared_ptr invokes its deleter if control-block allocation fails too.
    // Establish the count first so that path also has a matching decrement.
    active_control_calls_.fetch_add(1, std::memory_order_acq_rel);
    return std::shared_ptr<void>(state.get(), [state](void*) {
        state->active_control_calls_.fetch_sub(1, std::memory_order_acq_rel);
    });
}

void ActiveRuntimeState::reap_quarantined_workers() noexcept {
    bool descriptor_done = false;
    {
        std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
        descriptor_done = !descriptor_worker_started_;
    }
    if (descriptor_done && descriptor_worker_.joinable()) {
        descriptor_worker_.join();
    }
    if (pipeline_ != nullptr) {
        pipeline_->reap_completed_workers();
    }
}

Result<void> ActiveRuntimeState::supervise_binding_control(
    std::function<Result<void>()> operation, const char* reason) {
    auto guard = track_control_call();
    auto outcome = QuarantineManager::process().run(
        [guard, operation = std::move(operation)]() mutable { return operation(); },
        Clock::now() + binding_progress_timeout_, reason);
    if (!outcome.ok()) {
        return std::move(outcome).status();
    }
    switch (outcome.value().status) {
        case SupervisedCallStatus::Completed:
            return {};
        case SupervisedCallStatus::Failed:
            return std::move(outcome.value().error);
        case SupervisedCallStatus::TimedOut:
            control_quarantine_required_.store(true, std::memory_order_release);
            return runtime_status(StatusCode::Timeout, "Acquisition Binding control timed out");
    }
    return runtime_status(StatusCode::InternalError, "Unknown Binding control outcome");
}

Result<ActiveRuntimeState::DescriptorReadiness> ActiveRuntimeState::ensure_descriptor_ready(
    StreamDescriptorHandle descriptor) {
    if (!storage_control_.has_value()) {
        return runtime_status(StatusCode::InvalidState, "Storage control interface is unavailable");
    }
    const rodaq_interface control = storage_control_.value();
    const auto* table = static_cast<const rodaq_storage_control_v1*>(control.vtable);
    if (table->ensure_descriptor_ready == nullptr) {
        return runtime_status(
            StatusCode::InvalidArgument, "Storage descriptor readiness slot is missing");
    }

    std::shared_ptr<ActiveRuntimeState> state;
    try {
        state = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return runtime_status(StatusCode::InvalidState, "Runtime activation has no shared owner");
    }
    auto result = std::make_shared<std::optional<DescriptorReadiness>>();
    auto result_error = std::make_shared<Status>();
    const auto descriptor_value = rodaq_stream_descriptor_handle{descriptor.value()};
    auto guard = track_control_call();
    auto outcome = QuarantineManager::process().run(
        [control, table, descriptor_value, result, result_error, state, guard]() -> Result<void> {
            static_cast<void>(state);
            rodaq_descriptor_readiness_result readiness{};
            readiness.struct_size = sizeof(readiness);
            rodaq_status error{};
            const rodaq_status_code status = table->ensure_descriptor_ready(
                control.context, descriptor_value, &readiness, &error);
            if (status != RODAQ_STATUS_OK) {
                auto converted = status_from_abi(status, &error, "storage");
                *result_error = converted.ok() ? runtime_status(
                                                     StatusCode::InternalError,
                                                     "Storage descriptor readiness failed")
                                               : std::move(converted);
                return {};
            }
            if (readiness.struct_size < RODAQ_DESCRIPTOR_READINESS_RESULT_MIN_STRUCT_SIZE) {
                *result_error = runtime_status(
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
                                    : runtime_status(
                                          StatusCode::InvalidArgument,
                                          "Storage returned invalid descriptor readiness");
            }
            return {};
        },
        Clock::now() + descriptor_progress_timeout_, "Descriptor readiness");
    if (!outcome.ok()) {
        return std::move(outcome).status();
    }
    switch (outcome.value().status) {
        case SupervisedCallStatus::TimedOut:
            control_quarantine_required_.store(true, std::memory_order_release);
            return runtime_status(StatusCode::Timeout, "Storage descriptor readiness timed out");
        case SupervisedCallStatus::Failed:
            return std::move(outcome.value().error);
        case SupervisedCallStatus::Completed:
            break;
    }
    if (!result_error->ok()) {
        return std::move(*result_error);
    }
    if (!result->has_value()) {
        return runtime_status(
            StatusCode::InternalError, "Descriptor readiness produced no outcome");
    }
    return result->value();
}

Result<void> ActiveRuntimeState::initialize_pipeline(
    std::size_t effective_payload_limit, std::size_t maximum_payload_inflight_bytes,
    plugin::StoragePluginInstance* storage, std::chrono::nanoseconds progress_timeout) {
    if (pipeline_ != nullptr) {
        return runtime_status(StatusCode::InvalidState, "Runtime pipeline is already initialized");
    }
    const PayloadPoolConfig& pool_config = options_.resources.payload_pool;
    if (effective_payload_limit == 0 || pool_config.classes.empty() ||
        pool_config.classes.back().max_payload_bytes < effective_payload_limit) {
        return runtime_status(
            StatusCode::InvalidArgument,
            "Configured payload block size is below the effective payload limit");
    }
    try {
        pipeline_ = std::make_unique<StoragePipeline>(
            options_.resources.operation_capacity, options_.resources.completion_capacity,
            effective_payload_limit, maximum_payload_inflight_bytes, storage, progress_timeout,
            options_.resources.payload_release_workers,
            [this](Status error) { request_failure(std::move(error)); },
            [this](std::size_t bytes) noexcept {
                if (!stats_.record_payload_released(bytes)) {
                    request_failure(runtime_status(
                        StatusCode::InternalError, "Runtime inflight byte accounting underflowed"));
                }
            },
            [this](rodaq_storage_persistence_state persistence) noexcept {
                stats_.record_persistence(persistence);
                if (!stats_.record_operation_completed()) {
                    request_failure(runtime_status(
                        StatusCode::InternalError,
                        "Runtime inflight record accounting underflowed"));
                }
            },
            [this](StorageLatencyKind kind, std::int64_t duration_ns) noexcept {
                record_storage_latency(kind, duration_ns);
            },
            [this](
                const RuntimeRecord& record, rodaq_storage_operation operation,
                std::array<rodaq_timestamp_value, TimestampState::kMaxSlots>& values) {
                const BindingEntry* binding = bindings_.find_stable(record.binding());
                if (binding == nullptr) {
                    return Result<rodaq_storage_submission>(
                        runtime_status(StatusCode::InvalidState, "Record Binding is unavailable"));
                }
                const StreamDescriptorEntry* descriptor =
                    descriptors_.find_stable(binding->descriptor);
                if (descriptor == nullptr) {
                    return Result<rodaq_storage_submission>(runtime_status(
                        StatusCode::InvalidState, "Record Descriptor is unavailable"));
                }
                for (std::size_t slot = 0; slot < TimestampState::kMaxSlots; ++slot) {
                    values[slot] = {};
                    const auto reading = record.timestamps().value(slot);
                    if (reading.has_value()) {
                        values[slot].present = RODAQ_TRUE;
                        values[slot].reading = *reading;
                    } else {
                        values[slot].present = RODAQ_FALSE;
                    }
                }
                rodaq_storage_submission submission{};
                submission.struct_size = sizeof(submission);
                submission.operation = operation;
                submission.stream = rodaq_stream_handle{binding->stream.value()};
                submission.descriptor = rodaq_stream_descriptor_handle{binding->descriptor.value()};
                submission.acceptance_sequence = record.acceptance_sequence();
                if (record.source().has_value()) {
                    submission.source_present = RODAQ_TRUE;
                    submission.source.value = record.source()->value();
                }
                if (record.source_sequence().has_value()) {
                    submission.source_sequence_present = RODAQ_TRUE;
                    submission.source_sequence = *record.source_sequence();
                }
                submission.timestamp_values = values.data();
                submission.timestamp_value_count =
                    static_cast<std::uint32_t>(descriptor->canonical_timestamp_keys.size());
                const PayloadView payload = record.record_payload().view();
                submission.payload = rodaq_bytes_view{payload.data(), payload.size()};
                return Result<rodaq_storage_submission>(submission);
            });
        payload_pool_.set_capacity_changed_sink([this] { pipeline_->notify_admission_waiters(); });
        auto started = pipeline_->start();
        if (!started.ok()) {
            pipeline_.reset();
            return started;
        }
        return {};
    } catch (const std::exception& error) {
        return runtime_status(StatusCode::InternalError, error.what());
    }
}

Result<void> ActiveRuntimeState::register_acquisition_services(
    plugin::ServiceRegistry& registry, std::size_t plugin_index) {
    if (plugin_index >= acquisition_contexts_.size()) {
        return runtime_status(StatusCode::InvalidArgument, "Acquisition service index is invalid");
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

Result<void> ActiveRuntimeState::register_storage_services(plugin::ServiceRegistry& registry) {
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
        return runtime_status(StatusCode::InvalidState, "Runtime ingress Clock is unavailable");
    }
    result = registry.register_entry(
        RODAQ_RUNTIME_CLOCK_CORRELATION_SERVICE_ID, ingress_clock_correlation_->abi_interface());
    if (!result.ok()) {
        return result;
    }
    result = registry.register_entry(RODAQ_RUNTIME_STORAGE_SERVICE_ID, storage_service_interface());
    return result;
}

rodaq_interface ActiveRuntimeState::acquisition_service_interface(
    std::size_t plugin_index) noexcept {
    return rodaq_interface{
        acquisition_contexts_[plugin_index].get(),
        &kAcquisitionVtable,
    };
}

rodaq_interface ActiveRuntimeState::storage_service_interface() noexcept {
    return rodaq_interface{
        storage_context_.get(),
        &kStorageVtable,
    };
}

bool ActiveRuntimeState::mark_running() noexcept {
    std::lock_guard<std::mutex> control_lock(control_mutex_);
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    if (state_.load() != RuntimeState::Starting) {
        return false;
    }
    if (pipeline_ != nullptr) {
        pipeline_->open_admission();
    }
    gate_open_ = pipeline_ != nullptr;
    state_.store(RuntimeState::Running);
    admission_condition_.notify_all();
    return true;
}

void ActiveRuntimeState::mark_stopping() noexcept {
    if (!stop_started_valid_) {
        stop_started_ = Clock::now();
        stop_started_valid_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (state_.load() != RuntimeState::Failed) {
            state_.store(RuntimeState::Stopping);
        }
    }
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        if (state_.load() != RuntimeState::Failed) {
            state_.store(RuntimeState::Stopping);
        }
        gate_open_ = false;
    }
    if (pipeline_ != nullptr) {
        pipeline_->close_admission();
    }
    admission_condition_.notify_all();
}

void ActiveRuntimeState::mark_stopped() noexcept {
    record_stop_latency();
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        if (state_.load() != RuntimeState::Failed) {
            state_.store(RuntimeState::Stopped);
        }
        gate_open_ = false;
    }
    admission_condition_.notify_all();
}

void ActiveRuntimeState::mark_failed() noexcept {
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        state_.store(RuntimeState::Failed);
    }
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        state_.store(RuntimeState::Failed);
        gate_open_ = false;
    }
    if (pipeline_ != nullptr) {
        pipeline_->begin_failure_cutoff();
    }
    admission_condition_.notify_all();
}

bool ActiveRuntimeState::gate_open() const noexcept {
    std::lock_guard<std::mutex> lock(admission_mutex_);
    return gate_open_;
}

bool ActiveRuntimeState::begin_submission() noexcept {
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        ++active_submission_calls_;
    }
    return true;
}

void ActiveRuntimeState::end_submission() noexcept {
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        --active_submission_calls_;
    }
    admission_condition_.notify_all();
}

bool ActiveRuntimeState::wait_for_admission_drain(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(admission_mutex_);
    return admission_condition_.wait_until(lock, deadline, [this] {
        return active_submission_calls_ == 0 && admission_inflight_ == 0;
    });
}

bool ActiveRuntimeState::wait_for_submissions(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(admission_mutex_);
    return admission_condition_.wait_until(
        lock, deadline, [this] { return active_submission_calls_ == 0; });
}

void ActiveRuntimeState::request_failure(Status error) noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(failure_mutex_);
            if (primary_failure_.ok()) {
                primary_failure_ = std::move(error);
            } else if (secondary_failures_.size() < kMaxSecondaryFailures) {
                secondary_failures_.push_back(std::move(error));
            } else {
                ++secondary_overflow_count_;
            }
        }
        mark_failed();
        if (failure_signal_ != nullptr) {
            failure_signal_->notify();
        }
    } catch (...) {
    }
}

void ActiveRuntimeState::record_secondary_failure(Status error) noexcept {
    try {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        if (secondary_failures_.size() < kMaxSecondaryFailures) {
            secondary_failures_.push_back(std::move(error));
        } else {
            ++secondary_overflow_count_;
        }
    } catch (...) {
    }
}

void ActiveRuntimeState::log(
    LogLevel level, std::string_view category, std::string_view message) noexcept {
    if (logging_context_ != nullptr) {
        logging_context_->log(level, category, message);
    }
}

void ActiveRuntimeState::diagnostic(
    rodaq_diagnostic_severity severity, std::string_view code, std::string_view message) noexcept {
    LogLevel level = RODAQ_LOG_OFF;
    switch (severity) {
        case RODAQ_DIAGNOSTIC_INFO:
            level = RODAQ_LOG_INFO;
            break;
        case RODAQ_DIAGNOSTIC_WARNING:
            level = RODAQ_LOG_WARN;
            break;
        case RODAQ_DIAGNOSTIC_ERROR:
            level = RODAQ_LOG_ERROR;
            break;
        case RODAQ_DIAGNOSTIC_FATAL:
            level = RODAQ_LOG_CRITICAL;
            break;
        default:
            return;
    }
    try {
        const std::string text = std::string(code) + ": " + std::string(message);
        log(level, "rodaq.diagnostic", text);
    } catch (...) {
    }
}

bool ActiveRuntimeState::shutdown_logging(std::chrono::steady_clock::time_point deadline) noexcept {
    return logging_context_ == nullptr || logging_context_->shutdown(deadline);
}

void ActiveRuntimeState::quarantine_logging(std::string reason) noexcept {
    if (logging_context_ != nullptr) {
        logging_context_->quarantine_worker(std::move(reason));
    }
}

LoggingStats ActiveRuntimeState::current_logging_stats() const noexcept {
    return logging_context_ == nullptr ? LoggingStats{} : logging_context_->stats();
}

Result<PayloadPoolStats> ActiveRuntimeState::payload_pool_statistics() const {
    try {
        return payload_pool_.stats();
    } catch (const std::bad_alloc&) {
        return runtime_status(
            StatusCode::ResourceExhausted, "Payload statistics allocation failed");
    } catch (const std::exception& error) {
        return runtime_status(StatusCode::InternalError, error.what());
    }
}

bool ActiveRuntimeState::has_primary_failure() const noexcept {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return !primary_failure_.ok();
}

Result<void> ActiveRuntimeState::start_descriptor_reconciliation() {
    std::shared_ptr<ActiveRuntimeState> state;
    try {
        state = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return runtime_status(StatusCode::InvalidState, "Runtime activation has no shared owner");
    }

    std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
    if (descriptor_worker_started_ || descriptor_worker_.joinable()) {
        return runtime_status(
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
        return runtime_status(
            StatusCode::ResourceExhausted,
            std::string("Failed to start Descriptor reconciliation worker: ") + error.what());
    }
    return {};
}

Result<void> ActiveRuntimeState::request_descriptor_reconciliation(
    StreamDescriptorHandle descriptor, bool critical_for_start) {
    std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
    if (!descriptor_worker_started_ || descriptor_worker_closing_) {
        return runtime_status(
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
        return runtime_status(
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

Result<void> ActiveRuntimeState::wait_for_descriptor_reconciliation(
    std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(descriptor_worker_mutex_);
    if (!descriptor_worker_started_ && !descriptor_worker_.joinable()) {
        return runtime_status(StatusCode::InvalidState, "Descriptor reconciliation is not running");
    }
    if (!descriptor_worker_condition_.wait_until(lock, deadline, [this] {
            return descriptor_worker_count_ == 0 && !descriptor_worker_busy_;
        })) {
        return runtime_status(StatusCode::Timeout, "Descriptor reconciliation did not become idle");
    }
    if (!descriptor_worker_failure_.ok()) {
        return descriptor_worker_failure_;
    }
    return {};
}

bool ActiveRuntimeState::close_and_join_descriptor_reconciliation(
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
            request_failure(runtime_status(
                StatusCode::Timeout, "Descriptor reconciliation shutdown timed out"));
            return false;
        }
    }
    if (descriptor_worker_.joinable()) {
        descriptor_worker_.join();
    }
    return true;
}

void ActiveRuntimeState::run_descriptor_reconciliation() noexcept {
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
            failure = runtime_status(StatusCode::InternalError, error.what());
        } catch (...) {
            failure = runtime_status(StatusCode::InternalError, "Descriptor reconciliation failed");
        }

        bool fatal_failure = false;
        {
            std::lock_guard<std::mutex> lock(descriptor_worker_mutex_);
            if (!failure.ok() && (request.critical_for_start || requires_quarantine() ||
                                  state_.load() == RuntimeState::Failed)) {
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

Status ActiveRuntimeState::primary_failure() const {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return primary_failure_;
}

Result<std::vector<Status>> ActiveRuntimeState::secondary_failures() const {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    if (!primary_failure_.ok()) {
        return secondary_failures_;
    }
    return runtime_status(StatusCode::InvalidState, "Runtime has no primary failure");
}

std::uint64_t ActiveRuntimeState::secondary_failure_overflow_count() const noexcept {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return secondary_overflow_count_;
}

void ActiveRuntimeState::record_pre_rejected(bool gate_closed, bool timeout) noexcept {
    if (gate_closed) {
        stats_.record_gate_closed();
    } else if (timeout) {
        stats_.record_timed_out();
    } else {
        stats_.record_rejected();
    }
}

void ActiveRuntimeState::record_admission_wait(std::int64_t duration_ns) noexcept {
    if (duration_ns >= 0 && admission_wait_latency_ != nullptr) {
        static_cast<void>(admission_wait_latency_->record(duration_ns));
    }
}

void ActiveRuntimeState::record_storage_latency(
    StorageLatencyKind kind, std::int64_t duration_ns) noexcept {
    LatencyHistogram* histogram = nullptr;
    switch (kind) {
        case StorageLatencyKind::Handoff:
            histogram = storage_handoff_latency_.get();
            break;
        case StorageLatencyKind::PayloadConsumed:
            histogram = payload_consumed_latency_.get();
            break;
        case StorageLatencyKind::PersistenceCompleted:
            histogram = persistence_latency_.get();
            break;
    }
    if (histogram != nullptr && duration_ns >= 0) {
        static_cast<void>(histogram->record(duration_ns));
    }
}

void ActiveRuntimeState::record_stop_latency() noexcept {
    if (!stop_started_valid_ || stop_latency_ == nullptr) {
        return;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - stop_started_).count();
    if (elapsed >= 0) {
        static_cast<void>(stop_latency_->record(elapsed));
    }
    stop_started_valid_ = false;
}

void ActiveRuntimeState::fill_latency_stats(RuntimeStatsSnapshot& snapshot) const noexcept {
    if (admission_wait_latency_ != nullptr) {
        snapshot.latencies.admission_wait = latency_snapshot(*admission_wait_latency_);
    }
    if (storage_handoff_latency_ != nullptr) {
        snapshot.latencies.storage_handoff = latency_snapshot(*storage_handoff_latency_);
    }
    if (payload_consumed_latency_ != nullptr) {
        snapshot.latencies.payload_consumed = latency_snapshot(*payload_consumed_latency_);
    }
    if (persistence_latency_ != nullptr) {
        snapshot.latencies.persistence = latency_snapshot(*persistence_latency_);
    }
    if (stop_latency_ != nullptr) {
        snapshot.latencies.stop = latency_snapshot(*stop_latency_);
    }
}

Result<rodaq_runtime_lifecycle_snapshot> ActiveRuntimeState::lifecycle_snapshot() const {
    rodaq_runtime_lifecycle_snapshot snapshot{};
    const RuntimeState state = state_.load();
    switch (state) {
        case RuntimeState::Constructed:
        case RuntimeState::Configured:
            snapshot.state = RODAQ_RUNTIME_CONFIGURED;
            break;
        case RuntimeState::Starting:
            snapshot.state = RODAQ_RUNTIME_STARTING;
            break;
        case RuntimeState::Running:
            snapshot.state = RODAQ_RUNTIME_RUNNING;
            break;
        case RuntimeState::Stopping:
            snapshot.state = RODAQ_RUNTIME_STOPPING;
            break;
        case RuntimeState::Stopped:
            snapshot.state = RODAQ_RUNTIME_STOPPED;
            break;
        case RuntimeState::Failed:
            snapshot.state = RODAQ_RUNTIME_FAILED;
            break;
    }
    snapshot.normal_acquisition_allowed =
        state == RuntimeState::Running && gate_open() ? RODAQ_TRUE : RODAQ_FALSE;
    snapshot.generation = activation_generation_;
    return snapshot;
}

Result<void> ActiveRuntimeState::register_source(
    std::size_t plugin_index, const rodaq_source_registration& request, SourceHandle& handle) {
    if (!rodaq::plugin::is_valid_source_registration(request) ||
        plugin_index >= acquisition_health_.size()) {
        return runtime_status(StatusCode::InvalidArgument, "Source registration is invalid");
    }
    const auto* source_id = request.source_id.data;
    const std::size_t source_id_size = static_cast<std::size_t>(request.source_id.size);

    std::lock_guard<std::mutex> lock(control_mutex_);
    const RuntimeState current_state = state_.load();
    if (current_state != RuntimeState::Starting && current_state != RuntimeState::Running) {
        return runtime_status(StatusCode::InvalidState, "Source registration is not allowed");
    }
    if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
        RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return runtime_status(
            StatusCode::InvalidState, "Source registration requires an available Plugin");
    }
    for (SourceHandle existing : source_handles_) {
        const SourceEntry* entry = sources_.find(existing);
        if (entry != nullptr && bytes_equal(entry->source_id, source_id, source_id_size)) {
            if (entry->plugin_index != plugin_index) {
                return runtime_status(
                    StatusCode::Conflict, "Source identity belongs to another Plugin");
            }
            handle = existing;
            return {};
        }
    }

    auto availability = std::make_unique<std::atomic<std::uint32_t>>(RODAQ_AVAILABILITY_AVAILABLE);
    auto inserted = sources_.insert(SourceEntry{
        copy_bytes(arena_->resource(), source_id, source_id_size), plugin_index,
        std::move(availability)});
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    source_handles_.push_back(inserted.value());
    handle = inserted.value();
    return {};
}

Result<void> ActiveRuntimeState::register_clock(
    std::size_t plugin_index, const rodaq_clock_registration& request, ClockHandle& handle) {
    if (!rodaq::plugin::is_valid_clock_registration(request) ||
        plugin_index >= options_.acquisitions.size()) {
        return runtime_status(StatusCode::InvalidArgument, "Clock registration is invalid");
    }
    std::string_view origin;
    static_cast<void>(parse_abi_string(request.origin, &origin));
    const std::size_t native_size = static_cast<std::size_t>(request.native_id.size);

    std::lock_guard<std::mutex> lock(control_mutex_);
    const RuntimeState current_state = state_.load();
    if (current_state != RuntimeState::Starting && current_state != RuntimeState::Running) {
        return runtime_status(StatusCode::InvalidState, "Clock registration is not allowed");
    }
    if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
        RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return runtime_status(
            StatusCode::InvalidState, "Clock registration requires an available Plugin");
    }
    for (ClockHandle existing : clock_handles_) {
        const ClockEntry* entry = clocks_.find(existing);
        if (entry == nullptr || entry->plugin_index != plugin_index ||
            !bytes_equal(entry->native_id, request.native_id.data, native_size)) {
            continue;
        }
        if (entry->resolution_numerator != request.resolution_numerator ||
            entry->resolution_denominator != request.resolution_denominator ||
            !strings_equal(entry->origin, origin)) {
            return runtime_status(StatusCode::Conflict, "Clock registration facts conflict");
        }
        handle = existing;
        return {};
    }

    const std::uint64_t clock_id = next_clock_id_++;
    auto inserted = clocks_.insert(ClockEntry{
        copy_bytes(arena_->resource(), request.native_id.data, native_size),
        clock_id,
        request.resolution_numerator,
        request.resolution_denominator,
        copy_string(arena_->resource(), origin),
        plugin_index,
    });
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    clock_handles_.push_back(inserted.value());
    handle = inserted.value();
    return {};
}

Result<void> ActiveRuntimeState::register_schema(
    std::size_t plugin_index, const rodaq_schema_registration& request, SchemaHandle& handle) {
    if (!rodaq::plugin::is_valid_schema_registration(request) ||
        plugin_index >= acquisition_health_.size()) {
        return runtime_status(StatusCode::InvalidArgument, "Schema registration is invalid");
    }
    std::string_view format;
    std::string_view type_identifier;
    parse_abi_string(request.format, &format);
    if (request.type_identifier_present == RODAQ_TRUE) {
        parse_abi_string(request.type_identifier, &type_identifier);
    }
    const std::size_t representation_size = static_cast<std::size_t>(request.representation.size);

    std::lock_guard<std::mutex> lock(control_mutex_);
    const RuntimeState current_state = state_.load();
    if (current_state != RuntimeState::Starting && current_state != RuntimeState::Running) {
        return runtime_status(StatusCode::InvalidState, "Schema registration is not allowed");
    }
    if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
        RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return runtime_status(
            StatusCode::InvalidState, "Schema registration requires an available Plugin");
    }
    for (SchemaHandle existing : schema_handles_) {
        const SchemaEntry* entry = schemas_.find(existing);
        if (entry == nullptr || !strings_equal(entry->format, format) ||
            entry->type_identifier_present != (request.type_identifier_present == RODAQ_TRUE) ||
            !strings_equal(entry->type_identifier, type_identifier) ||
            !bytes_equal(entry->representation, request.representation.data, representation_size)) {
            continue;
        }
        handle = existing;
        return {};
    }

    const std::uint64_t schema_id = next_schema_id_++;
    auto inserted = schemas_.insert(SchemaEntry{
        schema_id,
        copy_string(arena_->resource(), format),
        request.type_identifier_present == RODAQ_TRUE,
        copy_string(arena_->resource(), type_identifier),
        copy_bytes(arena_->resource(), request.representation.data, representation_size),
    });
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    schema_handles_.push_back(inserted.value());
    handle = inserted.value();
    return {};
}

Result<void> ActiveRuntimeState::register_stream(
    std::size_t plugin_index, const rodaq_stream_registration& request, StreamHandle& handle) {
    if (!rodaq::plugin::is_valid_stream_registration(request) ||
        plugin_index >= options_.acquisitions.size()) {
        return runtime_status(StatusCode::InvalidArgument, "Stream registration is invalid");
    }
    const std::size_t native_size = static_cast<std::size_t>(request.native_id.size);

    std::lock_guard<std::mutex> lock(control_mutex_);
    const RuntimeState current_state = state_.load();
    if (current_state != RuntimeState::Starting && current_state != RuntimeState::Running) {
        return runtime_status(StatusCode::InvalidState, "Stream registration is not allowed");
    }
    if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
        RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return runtime_status(
            StatusCode::InvalidState, "Stream registration requires an available Plugin");
    }
    for (StreamHandle existing : stream_handles_) {
        const StreamEntry* entry = streams_.find(existing);
        if (entry != nullptr && entry->plugin_index == plugin_index &&
            bytes_equal(entry->native_id, request.native_id.data, native_size)) {
            handle = existing;
            return {};
        }
    }

    auto availability = std::make_unique<std::atomic<std::uint32_t>>(RODAQ_AVAILABILITY_AVAILABLE);
    auto current_binding = std::make_unique<std::atomic<std::uint64_t>>(0);
    auto admission_mutex = std::make_unique<std::mutex>();
    auto binding_transition_mutex = std::make_unique<std::mutex>();
    auto binding_transition_condition = std::make_unique<std::condition_variable>();
    auto inserted = streams_.insert(StreamEntry{
        next_stream_id_++,
        plugin_index,
        copy_bytes(arena_->resource(), request.native_id.data, native_size),
        std::move(availability),
        std::move(current_binding),
        std::move(admission_mutex),
        std::move(binding_transition_mutex),
        std::move(binding_transition_condition),
        false,
    });
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    stream_handles_.push_back(inserted.value());
    handle = inserted.value();
    return {};
}

Result<void> ActiveRuntimeState::publish_stream_descriptor(
    std::size_t plugin_index, const rodaq_stream_descriptor_registration& request,
    StreamDescriptorHandle& handle) {
    if (!rodaq::plugin::is_valid_stream_descriptor_registration(request) ||
        plugin_index >= options_.acquisitions.size()) {
        return runtime_status(
            StatusCode::InvalidArgument, "StreamDescriptor publication is invalid");
    }
    std::string_view name;
    std::string_view payload_encoding;
    parse_abi_string(request.name, &name);
    parse_abi_string(request.payload_encoding, &payload_encoding);

    bool critical_for_start = false;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        const RuntimeState current_state = state_.load();
        if (current_state != RuntimeState::Starting && current_state != RuntimeState::Running) {
            return runtime_status(
                StatusCode::InvalidState, "Descriptor publication is not allowed");
        }
        if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
            RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
            return runtime_status(
                StatusCode::InvalidState, "Descriptor publication requires an available Plugin");
        }
        critical_for_start = current_state == RuntimeState::Starting;
        const StreamHandle stream_handle = StreamHandle::from_raw(request.stream.value);
        StreamEntry* stream = streams_.find(stream_handle);
        if (stream == nullptr) {
            return runtime_status(StatusCode::NotFound, "Descriptor Stream is unknown");
        }
        if (stream->plugin_index != plugin_index) {
            return runtime_status(
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
                return runtime_status(StatusCode::InvalidArgument, "Timestamp role is invalid");
            }
            if (role == kIngressTimestampRole) {
                return runtime_status(
                    StatusCode::InvalidArgument,
                    "Acquisition Plugins must not publish the Runtime ingress timestamp role");
            }
            const ClockHandle clock = ClockHandle::from_raw(key.clock.value);
            const ClockEntry* clock_entry = clocks_.find(clock);
            if (clock_entry == nullptr) {
                return runtime_status(StatusCode::NotFound, "Timestamp Clock is unknown");
            }
            if (clock_entry->plugin_index != plugin_index) {
                return runtime_status(
                    StatusCode::InvalidState, "Timestamp Clock belongs to another owner");
            }
            for (std::size_t existing = 1; existing < keys.size(); ++existing) {
                if (keys[existing].clock == clock && strings_equal(keys[existing].role, role)) {
                    return runtime_status(
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
        for (TimestampKeySetHandle existing : timestamp_key_set_handles_) {
            const TimestampKeySetEntry* entry = timestamp_key_sets_.find(existing);
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
            key_set_id = next_timestamp_key_set_id_++;
            TimestampKeySetEntry entry{
                key_set_id,
                std::move(keys),
                std::move(abi_keys),
            };
            auto inserted = timestamp_key_sets_.insert(std::move(entry));
            if (!inserted.ok()) {
                return std::move(inserted).status();
            }
            key_set_handle = inserted.value();
            timestamp_key_set_handles_.push_back(key_set_handle);
        }
        TimestampKeySetEntry* key_set = timestamp_key_sets_.find(key_set_handle);
        if (key_set == nullptr) {
            return runtime_status(StatusCode::InternalError, "TimestampKeySet publication failed");
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
            next_descriptor_id_++,
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
            const SchemaEntry* schema = schemas_.find(descriptor_entry.schema);
            if (schema == nullptr) {
                return runtime_status(StatusCode::NotFound, "Descriptor Schema is unknown");
            }
            descriptor_entry.schema_id = schema->schema_id;
        }
        descriptor_entry.canonical_timestamp_keys.assign(
            key_set->abi_keys.begin(), key_set->abi_keys.end());

        auto descriptor_inserted = descriptors_.insert(std::move(descriptor_entry));
        if (!descriptor_inserted.ok()) {
            return std::move(descriptor_inserted).status();
        }
        handle = descriptor_inserted.value();
        descriptor_handles_.push_back(handle);
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

Result<bool> ActiveRuntimeState::reconcile_descriptor(StreamDescriptorHandle descriptor) {
    while (true) {
        StreamHandle stream;
        std::size_t plugin_index = 0;
        rodaq_interface control{};
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamDescriptorEntry* entry = descriptors_.find(descriptor);
            if (entry == nullptr) {
                return runtime_status(StatusCode::NotFound, "Descriptor is not published");
            }
            if (entry->lifecycle == DescriptorLifecycle::Active) {
                return true;
            }
            if (entry->lifecycle == DescriptorLifecycle::Failed ||
                entry->lifecycle == DescriptorLifecycle::Superseded) {
                return false;
            }
            const RuntimeState current_state = state_.load();
            if (current_state != RuntimeState::Starting && current_state != RuntimeState::Running) {
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
                return runtime_status(
                    StatusCode::InvalidState, "Acquisition control interface is unavailable");
            }
            control = acquisition_controls_[plugin_index].value();
        }

        auto readiness = ensure_descriptor_ready(descriptor);
        bool retry = false;
        if (!readiness.ok()) {
            {
                std::lock_guard<std::mutex> lock(control_mutex_);
                StreamDescriptorEntry* entry = descriptors_.find(descriptor);
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
                StreamDescriptorEntry* entry = descriptors_.find(descriptor);
                if (entry != nullptr) {
                    entry->reconciliation_running = false;
                    entry->lifecycle = DescriptorLifecycle::Unsupported;
                    retry = entry->reconciliation_retry_requested;
                    entry->reconciliation_retry_requested = false;
                    StreamEntry* stream_entry = streams_.find(entry->stream);
                    if (stream_entry != nullptr && stream_entry->current_binding->load() == 0) {
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
            StreamDescriptorEntry* entry = descriptors_.find(descriptor);
            const RuntimeState current_state = state_.load();
            if (entry == nullptr || (current_state != RuntimeState::Starting &&
                                     current_state != RuntimeState::Running)) {
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
            StreamDescriptorEntry* entry = descriptors_.find(descriptor);
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

Result<void> ActiveRuntimeState::activate_descriptor_binding(
    StreamDescriptorHandle descriptor, StreamHandle stream_handle, std::size_t plugin_index,
    rodaq_interface control) {
    StreamEntry* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        stream = streams_.find(stream_handle);
        if (stream == nullptr) {
            return runtime_status(StatusCode::NotFound, "Descriptor Stream is unavailable");
        }
    }

    std::unique_lock<std::mutex> transition_lock(*stream->binding_transition_mutex);
    stream->binding_transition_condition->wait(
        transition_lock, [stream] { return !stream->binding_transition_active; });
    stream->binding_transition_active = true;
    transition_lock.unlock();

    struct TransitionReset {
        StreamEntry* stream;
        ~TransitionReset() {
            {
                std::lock_guard<std::mutex> lock(*stream->binding_transition_mutex);
                stream->binding_transition_active = false;
                stream->binding_transition_token = 0;
            }
            stream->binding_transition_condition->notify_one();
        }
    } transition_reset{stream};

    BindingHandle candidate;
    BindingHandle old_binding;
    std::uint64_t old_binding_value = 0;
    std::uint64_t descriptor_generation = 0;
    rodaq_stream_descriptor_handle old_descriptor{};
    rodaq_binding_transition transition{};
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        StreamDescriptorEntry* descriptor_entry = descriptors_.find(descriptor);
        StreamEntry* current_stream = streams_.find(stream_handle);
        const RuntimeState current_state = state_.load();
        if (descriptor_entry == nullptr || current_stream == nullptr ||
            descriptor_entry->lifecycle != DescriptorLifecycle::Ready ||
            (current_state != RuntimeState::Starting && current_state != RuntimeState::Running)) {
            return runtime_status(StatusCode::InvalidState, "Descriptor is no longer ready");
        }
        descriptor_generation = descriptor_entry->stream_descriptor_id;
        old_binding_value = current_stream->current_binding->load(std::memory_order_acquire);
        if (old_binding_value != 0) {
            BindingEntry* current_binding =
                bindings_.find(BindingHandle::from_raw(old_binding_value));
            StreamDescriptorEntry* current_descriptor =
                current_binding == nullptr ? nullptr
                                           : descriptors_.find(current_binding->descriptor);
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
        auto created = create_binding_locked(
            plugin_index, stream_handle, descriptor, BindingLifecycle::Prepared);
        if (!created.ok()) {
            descriptor_entry->lifecycle = DescriptorLifecycle::Failed;
            return std::move(created).status();
        }
        candidate = created.value();
        descriptor_entry->lifecycle = DescriptorLifecycle::BindingPrepared;
        transition = rodaq_binding_transition{next_transition_id_++};
    }
    {
        std::lock_guard<std::mutex> lock(*stream->binding_transition_mutex);
        stream->binding_transition_token = transition.value;
    }

    const auto* table = static_cast<const rodaq_acquisition_control_v1*>(control.vtable);
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
        BindingEntry* prepared = bindings_.find(candidate);
        if (prepared != nullptr) {
            prepared->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
        }
        StreamDescriptorEntry* descriptor_entry = descriptors_.find(descriptor);
        if (descriptor_entry != nullptr) {
            descriptor_entry->lifecycle = DescriptorLifecycle::Failed;
        }
        return prepared_control;
    }
    if (*prepare_status != RODAQ_STATUS_OK) {
        bool resume_old = false;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamEntry* current_stream = streams_.find(stream_handle);
            const RuntimeState current_state = state_.load();
            resume_old = old_binding.valid() && current_stream != nullptr &&
                         current_state == RuntimeState::Running &&
                         current_stream->current_binding->load(std::memory_order_acquire) ==
                             old_binding_value &&
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
            BindingEntry* prepared = bindings_.find(candidate);
            if (prepared != nullptr) {
                prepared->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
            }
            StreamDescriptorEntry* descriptor_entry = descriptors_.find(descriptor);
            if (descriptor_entry != nullptr) {
                descriptor_entry->lifecycle = DescriptorLifecycle::Failed;
            }
            StreamEntry* current_stream = streams_.find(stream_handle);
            if (!resume_old && current_stream != nullptr) {
                current_stream->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
            }
        }
        if (!aborted.ok()) {
            request_failure(std::move(aborted).status());
        } else if (*abort_status != RODAQ_STATUS_OK) {
            request_failure(runtime_status(
                StatusCode::InternalError, "Binding abort after prepare failure also failed"));
        }
        auto converted = status_from_abi(*prepare_status, prepare_error.get(), "acquisition");
        return converted.ok() ? runtime_status(StatusCode::Unavailable, "Binding prepare failed")
                              : std::move(converted);
    }

    bool transition_stale = false;
    {
        std::lock_guard<std::mutex> stream_lock(*stream->admission_mutex);
        std::lock_guard<std::mutex> control_lock(control_mutex_);
        bool transition_token_current = false;
        {
            std::lock_guard<std::mutex> transition_guard(*stream->binding_transition_mutex);
            transition_token_current = stream->binding_transition_active &&
                                       stream->binding_transition_token == transition.value;
        }
        StreamDescriptorEntry* descriptor_entry = descriptors_.find(descriptor);
        BindingEntry* prepared = bindings_.find(candidate);
        StreamEntry* current_stream = streams_.find(stream_handle);
        BindingEntry* old = old_binding.valid() ? bindings_.find(old_binding) : nullptr;
        const RuntimeState current_state = state_.load();
        transition_stale =
            descriptor_entry == nullptr || prepared == nullptr || current_stream == nullptr ||
            descriptor_entry->stream_descriptor_id != descriptor_generation ||
            descriptor_entry->lifecycle != DescriptorLifecycle::BindingPrepared ||
            current_stream->current_binding->load(std::memory_order_acquire) != old_binding_value ||
            (old_binding.valid() && old == nullptr) || !transition_token_current ||
            (current_state != RuntimeState::Starting && current_state != RuntimeState::Running);
        if (transition_stale) {
            if (prepared != nullptr) {
                prepared->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
            }
            if (descriptor_entry != nullptr) {
                descriptor_entry->lifecycle =
                    current_state == RuntimeState::Stopping || current_state == RuntimeState::Failed
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
            current_stream->current_binding->store(candidate.value(), std::memory_order_release);
            current_stream->availability->store(RODAQ_AVAILABILITY_AVAILABLE);
            descriptor_entry->lifecycle = DescriptorLifecycle::Active;
        }
    }
    notify_admission_eligibility_changed();
    if (transition_stale) {
        const rodaq_binding_abort_disposition disposition =
            old_binding.valid() && state_.load() == RuntimeState::Running
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
                aborted.ok() ? runtime_status(
                                   StatusCode::Unavailable, "Binding stale-transition abort failed")
                             : aborted.status());
        }
        return runtime_status(StatusCode::InvalidState, "Binding transition became stale");
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
            StreamEntry* current_stream = streams_.find(stream_handle);
            if (current_stream != nullptr) {
                current_stream->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
            }
        }
        request_failure(
            committed.ok()
                ? runtime_status(
                      StatusCode::Unavailable, "Binding commit failed after Runtime publication")
                : committed.status());
        auto converted = committed.ok()
                             ? status_from_abi(*commit_status, commit_error.get(), "acquisition")
                             : committed.status();
        return converted.ok() ? runtime_status(StatusCode::Unavailable, "Binding commit failed")
                              : std::move(converted);
    }
    if (old_binding.valid()) {
        std::lock_guard<std::mutex> lock(control_mutex_);
        BindingEntry* old = bindings_.find(old_binding);
        if (old != nullptr && old->admission_inflight->load() == 0) {
            old->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
        }
    }
    return {};
}

void ActiveRuntimeState::on_projection_registered(
    rodaq_stream_descriptor_handle descriptor_value, std::string_view) noexcept {
    try {
        const StreamDescriptorHandle descriptor =
            StreamDescriptorHandle::from_raw(descriptor_value.value);
        bool critical_for_start = false;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamDescriptorEntry* entry = descriptors_.find(descriptor);
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
            const RuntimeState current_state = state_.load();
            if (current_state != RuntimeState::Starting && current_state != RuntimeState::Running) {
                return;
            }
            critical_for_start = current_state == RuntimeState::Starting;
        }
        auto result = request_descriptor_reconciliation(descriptor, critical_for_start);
        if (!result.ok()) {
            if (result.status().status_code() == StatusCode::ResourceExhausted) {
                request_failure(result.status());
            }
        }
    } catch (const std::exception& error) {
        request_failure(runtime_status(StatusCode::InternalError, error.what()));
    } catch (...) {
        request_failure(
            runtime_status(StatusCode::InternalError, "Descriptor reconciliation failed"));
    }
}

Result<BindingHandle> ActiveRuntimeState::create_binding_locked(
    std::size_t plugin_index, StreamHandle stream, StreamDescriptorHandle descriptor,
    BindingLifecycle lifecycle) {
    auto entry = BindingEntry{
        stream,
        descriptor,
        plugin_index,
        std::make_unique<std::atomic<std::uint32_t>>(static_cast<std::uint32_t>(lifecycle)),
        std::make_unique<std::atomic<std::uint64_t>>(0),
    };
    auto inserted = bindings_.insert(std::move(entry));
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    binding_handles_.push_back(inserted.value());
    return inserted.value();
}

void ActiveRuntimeState::retire_draining_binding(BindingHandle binding) noexcept {
    std::lock_guard<std::mutex> lock(control_mutex_);
    BindingEntry* entry = bindings_.find(binding);
    if (entry != nullptr &&
        entry->lifecycle->load() == static_cast<std::uint32_t>(BindingLifecycle::Draining) &&
        entry->admission_inflight->load() == 0) {
        entry->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
    }
}

void ActiveRuntimeState::notify_admission_eligibility_changed() noexcept {
    admission_eligibility_generation_.fetch_add(1, std::memory_order_acq_rel);
    if (pipeline_ != nullptr) {
        pipeline_->notify_admission_waiters();
    }
}

Result<void> ActiveRuntimeState::report_source_availability(
    std::size_t plugin_index, SourceHandle source, rodaq_availability_state availability) {
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        SourceEntry* entry = sources_.find(source);
        if (entry == nullptr) {
            return runtime_status(StatusCode::NotFound, "Source handle is unknown");
        }
        if (entry->plugin_index != plugin_index) {
            return runtime_status(StatusCode::InvalidState, "Source belongs to another Plugin");
        }
        if (availability != RODAQ_AVAILABILITY_AVAILABLE &&
            availability != RODAQ_AVAILABILITY_UNAVAILABLE) {
            return runtime_status(StatusCode::InvalidArgument, "Source availability is invalid");
        }
        entry->availability->store(availability);
    }
    notify_admission_eligibility_changed();
    return {};
}

Result<void> ActiveRuntimeState::report_stream_availability(
    std::size_t plugin_index, StreamHandle stream, rodaq_availability_state availability) {
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        StreamEntry* entry = streams_.find(stream);
        if (entry == nullptr) {
            return runtime_status(StatusCode::NotFound, "Stream handle is unknown");
        }
        if (entry->plugin_index != plugin_index) {
            return runtime_status(StatusCode::InvalidState, "Stream belongs to another Plugin");
        }
        if (availability != RODAQ_AVAILABILITY_AVAILABLE &&
            availability != RODAQ_AVAILABILITY_UNAVAILABLE) {
            return runtime_status(StatusCode::InvalidArgument, "Stream availability is invalid");
        }
        entry->availability->store(availability);
    }
    notify_admission_eligibility_changed();
    return {};
}

Result<void> ActiveRuntimeState::report_plugin_state(
    std::size_t plugin_index, rodaq_acquisition_plugin_state plugin_state,
    const rodaq_status* diagnostic) {
    if (plugin_index >= acquisition_health_.size() ||
        (plugin_state != RODAQ_ACQUISITION_PLUGIN_AVAILABLE &&
         plugin_state != RODAQ_ACQUISITION_PLUGIN_RECOVERABLE_FAILURE &&
         plugin_state != RODAQ_ACQUISITION_PLUGIN_TERMINAL_FAILURE)) {
        return runtime_status(StatusCode::InvalidArgument, "Plugin state is invalid");
    }

    bool notify_eligibility = false;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        const std::uint32_t previous =
            acquisition_health_[plugin_index]->load(std::memory_order_acquire);
        if (previous == RODAQ_ACQUISITION_PLUGIN_TERMINAL_FAILURE &&
            plugin_state != RODAQ_ACQUISITION_PLUGIN_TERMINAL_FAILURE) {
            return runtime_status(
                StatusCode::InvalidState, "Terminal Plugin failure cannot recover");
        }
        acquisition_health_[plugin_index]->store(plugin_state, std::memory_order_release);

        if (plugin_state != RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
            for (SourceHandle handle : source_handles_) {
                SourceEntry* source = sources_.find(handle);
                if (source != nullptr && source->plugin_index == plugin_index) {
                    source->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
                }
            }
            for (StreamHandle handle : stream_handles_) {
                StreamEntry* stream = streams_.find(handle);
                if (stream == nullptr || stream->plugin_index != plugin_index) {
                    continue;
                }
                stream->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
                stream->current_binding->store(0, std::memory_order_release);
            }
            for (BindingHandle handle : binding_handles_) {
                BindingEntry* binding = bindings_.find(handle);
                if (binding != nullptr && binding->plugin_index == plugin_index) {
                    binding->lifecycle->store(
                        static_cast<std::uint32_t>(BindingLifecycle::Retired),
                        std::memory_order_release);
                }
            }
            for (StreamDescriptorHandle handle : descriptor_handles_) {
                StreamDescriptorEntry* descriptor = descriptors_.find(handle);
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
                ? runtime_status(
                      StatusCode::Unavailable, "Acquisition Plugin reported terminal failure")
                : status_from_abi_diagnostic(*diagnostic));
    }
    return {};
}

Result<void> ActiveRuntimeState::get_source(SourceHandle handle, rodaq_source_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const SourceEntry* entry = sources_.find(handle);
    if (entry == nullptr) {
        return runtime_status(StatusCode::NotFound, "Source handle was not found");
    }
    view.struct_size = sizeof(view);
    view.handle = rodaq_source_handle{handle.value()};
    view.source_id = rodaq_bytes_view{entry->source_id.data(), entry->source_id.size()};
    return {};
}

Result<void> ActiveRuntimeState::get_clock(ClockHandle handle, rodaq_clock_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const ClockEntry* entry = clocks_.find(handle);
    if (entry == nullptr) {
        return runtime_status(StatusCode::NotFound, "Clock handle was not found");
    }
    view.struct_size = sizeof(view);
    view.handle = rodaq_clock_handle{handle.value()};
    view.clock_id = entry->clock_id;
    view.resolution_numerator = entry->resolution_numerator;
    view.resolution_denominator = entry->resolution_denominator;
    view.origin = rodaq_string_view{entry->origin.data(), entry->origin.size()};
    return {};
}

Result<void> ActiveRuntimeState::get_schema(SchemaHandle handle, rodaq_schema_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const SchemaEntry* entry = schemas_.find(handle);
    if (entry == nullptr) {
        return runtime_status(StatusCode::NotFound, "Schema handle was not found");
    }
    view.struct_size = sizeof(view);
    view.type_identifier_present = entry->type_identifier_present ? RODAQ_TRUE : RODAQ_FALSE;
    view.handle = rodaq_schema_handle{handle.value()};
    view.schema_id = entry->schema_id;
    view.format = rodaq_string_view{entry->format.data(), entry->format.size()};
    view.type_identifier =
        rodaq_string_view{entry->type_identifier.data(), entry->type_identifier.size()};
    view.representation =
        rodaq_bytes_view{entry->representation.data(), entry->representation.size()};
    return {};
}

Result<void> ActiveRuntimeState::get_timestamp_key_set(
    std::uint64_t id, rodaq_timestamp_key_set_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    for (TimestampKeySetHandle handle : timestamp_key_set_handles_) {
        const TimestampKeySetEntry* entry = timestamp_key_sets_.find(handle);
        if (entry == nullptr || entry->timestamp_key_set_id != id) {
            continue;
        }
        view.struct_size = sizeof(view);
        view.timestamp_key_set_id = id;
        view.keys = entry->abi_keys.data();
        view.key_count = static_cast<std::uint32_t>(entry->abi_keys.size());
        return {};
    }
    return runtime_status(StatusCode::NotFound, "TimestampKeySet was not found");
}

Result<void> ActiveRuntimeState::get_stream(StreamHandle handle, rodaq_stream_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const StreamEntry* entry = streams_.find(handle);
    if (entry == nullptr) {
        return runtime_status(StatusCode::NotFound, "Stream handle was not found");
    }
    view.struct_size = sizeof(view);
    view.handle = rodaq_stream_handle{handle.value()};
    view.stream_id = entry->stream_id;
    return {};
}

Result<void> ActiveRuntimeState::get_stream_descriptor(
    StreamDescriptorHandle handle, rodaq_stream_descriptor_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const StreamDescriptorEntry* entry = descriptors_.find(handle);
    if (entry == nullptr) {
        return runtime_status(StatusCode::NotFound, "StreamDescriptor handle was not found");
    }
    view.struct_size = sizeof(view);
    view.schema_present = entry->schema_present ? RODAQ_TRUE : RODAQ_FALSE;
    view.handle = rodaq_stream_descriptor_handle{handle.value()};
    view.stream = rodaq_stream_handle{entry->stream.value()};
    view.schema = rodaq_schema_handle{entry->schema.value()};
    view.stream_descriptor_id = entry->stream_descriptor_id;
    const StreamEntry* stream = streams_.find(entry->stream);
    view.stream_id = stream == nullptr ? 0 : stream->stream_id;
    view.name = rodaq_string_view{entry->name.data(), entry->name.size()};
    view.schema_id = entry->schema_id;
    view.payload_encoding =
        rodaq_string_view{entry->payload_encoding.data(), entry->payload_encoding.size()};
    view.timestamp_key_set_id = entry->timestamp_key_set_id;
    view.transport_timestamp_keys = entry->transport_timestamp_keys.data();
    view.transport_timestamp_key_count =
        static_cast<std::uint32_t>(entry->transport_timestamp_keys.size());
    return {};
}

namespace {

struct SubmissionCallGuard {
    explicit SubmissionCallGuard(ActiveRuntimeState& runtime_state) noexcept
    : state_(&runtime_state) {
        runtime_state.begin_submission();
    }
    ~SubmissionCallGuard() { state_->end_submission(); }

    ActiveRuntimeState* state_;
};

rodaq_acquisition_submit_result pre_rejected(
    ActiveRuntimeState& state, rodaq_admission_rejection_reason reason,
    rodaq_payload_disposition disposition, bool gate_closed = false,
    bool timeout = false) noexcept {
    state.record_pre_rejected(gate_closed, timeout);
    return submit_result(RODAQ_ADMISSION_PRE_REJECTED, reason, disposition);
}

}  // namespace

Result<rodaq_acquisition_submit_result> ActiveRuntimeState::submit(
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
    if (state_.load() != RuntimeState::Running || !gate_open() || pipeline_ == nullptr) {
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
            if (state_.load() != RuntimeState::Running || !gate_open()) {
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
        if (state_.load() != RuntimeState::Running || !gate_open_) {
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
            deferred_failure = runtime_status(
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
                auto record = RuntimeRecord::create(
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

RuntimeConfig& ActiveRuntimeState::options() noexcept { return options_; }
const RuntimeConfig& ActiveRuntimeState::options() const noexcept { return options_; }
std::uint64_t ActiveRuntimeState::activation_generation() const noexcept {
    return activation_generation_;
}
StoragePipeline* ActiveRuntimeState::pipeline() noexcept { return pipeline_.get(); }
std::optional<plugin::StoragePluginInstance>& ActiveRuntimeState::storage_instance() noexcept {
    return storage_instance_;
}
std::optional<plugin::AcquisitionPluginInstance>& ActiveRuntimeState::acquisition_instance(
    std::size_t index) noexcept {
    return acquisition_instances_[index];
}
RuntimeState ActiveRuntimeState::state() const noexcept { return state_.load(); }
bool ActiveRuntimeState::requires_quarantine() const noexcept {
    return control_quarantine_required_.load(std::memory_order_acquire) &&
           active_control_calls_.load(std::memory_order_acquire) != 0;
}
void ActiveRuntimeState::set_failure_signal(std::shared_ptr<FailureSignal> signal) noexcept {
    failure_signal_ = std::move(signal);
}
RuntimeStatsSnapshot ActiveRuntimeState::stats() const noexcept {
    RuntimeStatsSnapshot snapshot = stats_.snapshot();
    snapshot.logging = current_logging_stats();
    fill_latency_stats(snapshot);
    return snapshot;
}

}  // namespace rodaq::runtime
