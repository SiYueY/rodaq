#include "runtime/active_runtime_state.hpp"

#include "plugin/plugin_error.hpp"
#include "runtime/quarantine.hpp"

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

Error runtime_error(ErrorCode code, std::string message) {
    return Error(code, "runtime", 0, std::move(message));
}

template <typename T, typename HandleType>
Registry<T, HandleType> make_registry(std::size_t capacity, std::uint32_t generation) {
    auto result = Registry<T, HandleType>::Create(capacity, generation);
    if (!result.has_value()) {
        throw std::runtime_error(std::string(result.error().message()));
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
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InternalError, "Runtime ingress clock reading overflowed"));
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
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Runtime output pointer is null"));
    }
    const std::uint32_t capacity = destination->struct_size;
    const std::uint32_t initialized = initialized_prefix_size<Struct>(capacity);
    std::memset(destination, 0, initialized);
    std::memcpy(destination, &output, initialized);
    destination->struct_size = initialized;
    return {};
}

Error from_abi_error(const rodaq_error& error) {
    const std::string_view domain(
        error.domain, std::min<std::size_t>(error.domain_size, sizeof(error.domain)));
    const std::string_view message(
        error.message, std::min<std::size_t>(error.message_size, sizeof(error.message)));
    ErrorCode code = ErrorCode::InternalError;
    switch (error.code) {
        case RODAQ_STATUS_INVALID_ARGUMENT:
            code = ErrorCode::InvalidArgument;
            break;
        case RODAQ_STATUS_INVALID_STATE:
            code = ErrorCode::InvalidState;
            break;
        case RODAQ_STATUS_NOT_FOUND:
            code = ErrorCode::NotFound;
            break;
        case RODAQ_STATUS_CONFLICT:
            code = ErrorCode::Conflict;
            break;
        case RODAQ_STATUS_RESOURCE_EXHAUSTED:
            code = ErrorCode::ResourceExhausted;
            break;
        case RODAQ_STATUS_UNAVAILABLE:
            code = ErrorCode::Unavailable;
            break;
        case RODAQ_STATUS_STORAGE_ERROR:
            code = ErrorCode::StorageError;
            break;
        case RODAQ_STATUS_TIMEOUT:
            code = ErrorCode::Timeout;
            break;
        default:
            code = ErrorCode::InternalError;
            break;
    }
    return Error(code, std::string(domain), error.native_code, std::string(message));
}

rodaq_status assign_status(const Error& error, rodaq_error* out_error) noexcept {
    assign_rodaq_error(error, out_error);
    return status_from_error_code(error.code());
}

rodaq_status assign_message(
    ErrorCode code, std::string_view message, rodaq_error* out_error) noexcept {
    assign_rodaq_error(status_from_error_code(code), "runtime", 0, message, out_error);
    return status_from_error_code(code);
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

void common_log(void*, rodaq_log_level, rodaq_string_view, rodaq_string_view) noexcept {}

std::uint64_t common_monotonic_time_ns(void*) noexcept {
    const auto now = Clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

rodaq_status common_lifecycle_snapshot(
    void* context, rodaq_runtime_lifecycle_snapshot* out_snapshot,
    rodaq_error* out_error) noexcept {
    if (context == nullptr || out_snapshot == nullptr) {
        return assign_message(
            ErrorCode::InvalidArgument, "lifecycle snapshot input is invalid", out_error);
    }
    try {
        *out_snapshot = {};
        const auto result = state_from(context)->LifecycleSnapshot();
        if (!result.has_value()) {
            return assign_status(result.error(), out_error);
        }
        *out_snapshot = result.value();
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(ErrorCode::InternalError, error.what(), out_error);
    } catch (...) {
        return assign_message(ErrorCode::InternalError, "lifecycle snapshot failed", out_error);
    }
}

void common_diagnostic(
    void*, rodaq_diagnostic_severity, rodaq_string_view, rodaq_string_view) noexcept {}

template <typename View, typename Getter>
rodaq_status canonical_view(
    void* context, View* out_view, rodaq_error* out_error, Getter&& getter) noexcept {
    if (context == nullptr || out_view == nullptr) {
        return assign_message(
            ErrorCode::InvalidArgument, "canonical view input is null", out_error);
    }
    try {
        const auto result = getter(*state_from(context));
        if (!result.has_value()) {
            return assign_status(result.error(), out_error);
        }
        const auto copied = write_output_prefix(result.value(), out_view);
        return copied.has_value() ? RODAQ_STATUS_OK : assign_status(copied.error(), out_error);
    } catch (const std::exception& error) {
        return assign_message(ErrorCode::InternalError, error.what(), out_error);
    } catch (...) {
        return assign_message(ErrorCode::InternalError, "canonical view failed", out_error);
    }
}

rodaq_status canonical_get_source(
    void* context, rodaq_source_handle handle, rodaq_source_view* out_view,
    rodaq_error* out_error) noexcept {
    return canonical_view(context, out_view, out_error, [handle](ActiveRuntimeState& state) {
        rodaq_source_view view{};
        view.struct_size = sizeof(view);
        auto result = state.GetSource(SourceHandle::FromRaw(handle.value), view);
        if (!result.has_value()) {
            return Result<rodaq_source_view>(nonstd::make_unexpected(std::move(result.error())));
        }
        return Result<rodaq_source_view>(view);
    });
}

rodaq_status canonical_get_clock(
    void* context, rodaq_clock_handle handle, rodaq_clock_view* out_view,
    rodaq_error* out_error) noexcept {
    return canonical_view(context, out_view, out_error, [handle](ActiveRuntimeState& state) {
        rodaq_clock_view view{};
        view.struct_size = sizeof(view);
        auto result = state.GetClock(ClockHandle::FromRaw(handle.value), view);
        if (!result.has_value()) {
            return Result<rodaq_clock_view>(nonstd::make_unexpected(std::move(result.error())));
        }
        return Result<rodaq_clock_view>(view);
    });
}

rodaq_status canonical_get_schema(
    void* context, rodaq_schema_handle handle, rodaq_schema_view* out_view,
    rodaq_error* out_error) noexcept {
    return canonical_view(context, out_view, out_error, [handle](ActiveRuntimeState& state) {
        rodaq_schema_view view{};
        view.struct_size = sizeof(view);
        auto result = state.GetSchema(SchemaHandle::FromRaw(handle.value), view);
        if (!result.has_value()) {
            return Result<rodaq_schema_view>(nonstd::make_unexpected(std::move(result.error())));
        }
        return Result<rodaq_schema_view>(view);
    });
}

rodaq_status canonical_get_timestamp_key_set(
    void* context, std::uint64_t id, rodaq_timestamp_key_set_view* out_view,
    rodaq_error* out_error) noexcept {
    return canonical_view(context, out_view, out_error, [id](ActiveRuntimeState& state) {
        rodaq_timestamp_key_set_view view{};
        view.struct_size = sizeof(view);
        auto result = state.GetTimestampKeySet(id, view);
        if (!result.has_value()) {
            return Result<rodaq_timestamp_key_set_view>(
                nonstd::make_unexpected(std::move(result.error())));
        }
        return Result<rodaq_timestamp_key_set_view>(view);
    });
}

rodaq_status canonical_get_stream(
    void* context, rodaq_stream_handle handle, rodaq_stream_view* out_view,
    rodaq_error* out_error) noexcept {
    return canonical_view(context, out_view, out_error, [handle](ActiveRuntimeState& state) {
        rodaq_stream_view view{};
        view.struct_size = sizeof(view);
        auto result = state.GetStream(StreamHandle::FromRaw(handle.value), view);
        if (!result.has_value()) {
            return Result<rodaq_stream_view>(nonstd::make_unexpected(std::move(result.error())));
        }
        return Result<rodaq_stream_view>(view);
    });
}

rodaq_status canonical_get_stream_descriptor(
    void* context, rodaq_stream_descriptor_handle handle, rodaq_stream_descriptor_view* out_view,
    rodaq_error* out_error) noexcept {
    return canonical_view(context, out_view, out_error, [handle](ActiveRuntimeState& state) {
        rodaq_stream_descriptor_view view{};
        view.struct_size = sizeof(view);
        auto result =
            state.GetStreamDescriptor(StreamDescriptorHandle::FromRaw(handle.value), view);
        if (!result.has_value()) {
            return Result<rodaq_stream_descriptor_view>(
                nonstd::make_unexpected(std::move(result.error())));
        }
        return Result<rodaq_stream_descriptor_view>(view);
    });
}

rodaq_status acquisition_register_source(
    void* context, const rodaq_source_registration* request, rodaq_source_handle* out_handle,
    rodaq_error* out_error) noexcept {
    if (context == nullptr || request == nullptr || out_handle == nullptr) {
        return assign_message(
            ErrorCode::InvalidArgument, "source registration input is null", out_error);
    }
    *out_handle = {};
    try {
        auto* service_context = acquisition_context_from(context);
        SourceHandle handle;
        const auto result =
            service_context->state->RegisterSource(service_context->plugin_index, *request, handle);
        if (!result.has_value()) {
            return assign_status(result.error(), out_error);
        }
        out_handle->value = handle.Value();
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(ErrorCode::InternalError, error.what(), out_error);
    } catch (...) {
        return assign_message(ErrorCode::InternalError, "source registration failed", out_error);
    }
}

rodaq_status acquisition_register_clock(
    void* context, const rodaq_clock_registration* request, rodaq_clock_handle* out_handle,
    rodaq_error* out_error) noexcept {
    if (context == nullptr || request == nullptr || out_handle == nullptr) {
        return assign_message(
            ErrorCode::InvalidArgument, "clock registration input is null", out_error);
    }
    *out_handle = {};
    try {
        auto* service_context = acquisition_context_from(context);
        ClockHandle handle;
        const auto result =
            service_context->state->RegisterClock(service_context->plugin_index, *request, handle);
        if (!result.has_value()) {
            return assign_status(result.error(), out_error);
        }
        out_handle->value = handle.Value();
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(ErrorCode::InternalError, error.what(), out_error);
    } catch (...) {
        return assign_message(ErrorCode::InternalError, "clock registration failed", out_error);
    }
}

rodaq_status acquisition_register_schema(
    void* context, const rodaq_schema_registration* request, rodaq_schema_handle* out_handle,
    rodaq_error* out_error) noexcept {
    if (context == nullptr || request == nullptr || out_handle == nullptr) {
        return assign_message(
            ErrorCode::InvalidArgument, "schema registration input is null", out_error);
    }
    *out_handle = {};
    try {
        auto* service_context = acquisition_context_from(context);
        SchemaHandle handle;
        const auto result = service_context->state->RegisterSchema(*request, handle);
        if (!result.has_value()) {
            return assign_status(result.error(), out_error);
        }
        out_handle->value = handle.Value();
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(ErrorCode::InternalError, error.what(), out_error);
    } catch (...) {
        return assign_message(ErrorCode::InternalError, "schema registration failed", out_error);
    }
}

rodaq_status acquisition_register_stream(
    void* context, const rodaq_stream_registration* request, rodaq_stream_handle* out_handle,
    rodaq_error* out_error) noexcept {
    if (context == nullptr || request == nullptr || out_handle == nullptr) {
        return assign_message(
            ErrorCode::InvalidArgument, "stream registration input is null", out_error);
    }
    *out_handle = {};
    try {
        auto* service_context = acquisition_context_from(context);
        StreamHandle handle;
        const auto result =
            service_context->state->RegisterStream(service_context->plugin_index, *request, handle);
        if (!result.has_value()) {
            return assign_status(result.error(), out_error);
        }
        out_handle->value = handle.Value();
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(ErrorCode::InternalError, error.what(), out_error);
    } catch (...) {
        return assign_message(ErrorCode::InternalError, "stream registration failed", out_error);
    }
}

rodaq_status acquisition_publish_descriptor(
    void* context, const rodaq_stream_descriptor_registration* request,
    rodaq_stream_descriptor_handle* out_handle, rodaq_error* out_error) noexcept {
    if (context == nullptr || request == nullptr || out_handle == nullptr) {
        return assign_message(
            ErrorCode::InvalidArgument, "descriptor publication input is null", out_error);
    }
    *out_handle = {};
    try {
        auto* service_context = acquisition_context_from(context);
        StreamDescriptorHandle handle;
        const auto result = service_context->state->PublishStreamDescriptor(
            service_context->plugin_index, *request, handle);
        if (!result.has_value()) {
            return assign_status(result.error(), out_error);
        }
        out_handle->value = handle.Value();
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(ErrorCode::InternalError, error.what(), out_error);
    } catch (...) {
        return assign_message(ErrorCode::InternalError, "descriptor publication failed", out_error);
    }
}

rodaq_status acquisition_report_source(
    void* context, rodaq_source_handle source, rodaq_availability_state availability,
    const rodaq_error*, rodaq_error* out_error) noexcept {
    if (context == nullptr) {
        return assign_message(
            ErrorCode::InvalidArgument, "source report context is null", out_error);
    }
    try {
        const auto result =
            state_from(acquisition_context_from(context)->state)
                ->ReportSourceAvailability(SourceHandle::FromRaw(source.value), availability);
        return result.has_value() ? RODAQ_STATUS_OK : assign_status(result.error(), out_error);
    } catch (const std::exception& error) {
        return assign_message(ErrorCode::InternalError, error.what(), out_error);
    } catch (...) {
        return assign_message(ErrorCode::InternalError, "source report failed", out_error);
    }
}

rodaq_status acquisition_report_stream(
    void* context, rodaq_stream_handle stream, rodaq_availability_state availability,
    const rodaq_error*, rodaq_error* out_error) noexcept {
    if (context == nullptr) {
        return assign_message(
            ErrorCode::InvalidArgument, "stream report context is null", out_error);
    }
    try {
        const auto result =
            state_from(acquisition_context_from(context)->state)
                ->ReportStreamAvailability(StreamHandle::FromRaw(stream.value), availability);
        return result.has_value() ? RODAQ_STATUS_OK : assign_status(result.error(), out_error);
    } catch (const std::exception& error) {
        return assign_message(ErrorCode::InternalError, error.what(), out_error);
    } catch (...) {
        return assign_message(ErrorCode::InternalError, "stream report failed", out_error);
    }
}

rodaq_status acquisition_report_plugin_state(
    void* context, rodaq_acquisition_plugin_state plugin_state, const rodaq_error* diagnostic,
    rodaq_error* out_error) noexcept {
    if (context == nullptr || (plugin_state != RODAQ_ACQUISITION_PLUGIN_AVAILABLE &&
                               plugin_state != RODAQ_ACQUISITION_PLUGIN_RECOVERABLE_FAILURE &&
                               plugin_state != RODAQ_ACQUISITION_PLUGIN_TERMINAL_FAILURE)) {
        return assign_message(
            ErrorCode::InvalidArgument, "plugin state report is invalid", out_error);
    }
    if (plugin_state == RODAQ_ACQUISITION_PLUGIN_TERMINAL_FAILURE && diagnostic != nullptr) {
        state_from(acquisition_context_from(context)->state)
            ->RequestFailure(from_abi_error(*diagnostic));
    }
    return RODAQ_STATUS_OK;
}

rodaq_status acquisition_submit(
    void* context, const rodaq_record_submission* submission,
    rodaq_acquisition_submit_result* out_result, rodaq_error* out_error) noexcept {
    if (context == nullptr || submission == nullptr || out_result == nullptr) {
        return assign_message(ErrorCode::InvalidArgument, "submit input is null", out_error);
    }
    const std::uint32_t capacity = out_result->struct_size;
    if (capacity < RODAQ_ACQUISITION_SUBMIT_RESULT_MIN_STRUCT_SIZE) {
        return assign_message(
            ErrorCode::InvalidArgument, "submit result capacity is too small", out_error);
    }
    try {
        auto* service_context = acquisition_context_from(context);
        rodaq_payload_disposition disposition = RODAQ_PAYLOAD_DISPOSITION_NOT_APPLICABLE;
        const auto result =
            service_context->state->Submit(service_context->plugin_index, *submission, disposition);
        if (!result.has_value()) {
            return assign_status(result.error(), out_error);
        }
        const auto copied = write_output_prefix(result.value(), out_result);
        return copied.has_value() ? RODAQ_STATUS_OK : assign_status(copied.error(), out_error);
    } catch (const std::exception& error) {
        return assign_message(ErrorCode::InternalError, error.what(), out_error);
    } catch (...) {
        return assign_message(ErrorCode::InternalError, "submit failed", out_error);
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
    const rodaq_error* error) noexcept {
    if (context == nullptr) {
        return;
    }
    try {
        auto* state = state_from(storage_context_from(context)->state);
        if (state->pipeline() != nullptr) {
            state->pipeline()->persistence_completed(operation, state_value, error);
        }
    } catch (...) {
    }
}

void storage_report_failure(void* context, const rodaq_error* error) noexcept {
    if (context == nullptr) {
        return;
    }
    try {
        Error failure = error == nullptr
                            ? runtime_error(ErrorCode::StorageError, "Storage reported failure")
                            : from_abi_error(*error);
        state_from(storage_context_from(context)->state)->RequestFailure(std::move(failure));
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

Result<std::unique_ptr<ActiveRuntimeState>> ActiveRuntimeState::Create(
    RuntimeOptions options, std::uint64_t activation_generation) {
    try {
        return std::unique_ptr<ActiveRuntimeState>(
            new ActiveRuntimeState(std::move(options), activation_generation));
    } catch (const std::bad_alloc&) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::ResourceExhausted, "Runtime activation allocation failed"));
    } catch (const std::exception& error) {
        return nonstd::make_unexpected(runtime_error(ErrorCode::InternalError, error.what()));
    } catch (...) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InternalError, "Runtime activation construction failed"));
    }
}

ActiveRuntimeState::ActiveRuntimeState(RuntimeOptions options, std::uint64_t activation_generation)
: options_(std::move(options)),
  activation_generation_(activation_generation),
  arena_([this] {
      auto result = StructuralArena::Create(options_.structural_arena_bytes);
      if (!result.has_value()) {
          throw std::runtime_error(std::string(result.error().message()));
      }
      return std::move(result.value());
  }()),
  sources_(make_registry<SourceEntry, SourceHandle>(
      options_.max_sources, registry_generation(activation_generation))),
  clocks_(make_registry<ClockEntry, ClockHandle>(
      options_.max_clocks, registry_generation(activation_generation))),
  schemas_(make_registry<SchemaEntry, SchemaHandle>(
      options_.max_schemas, registry_generation(activation_generation))),
  timestamp_key_sets_(make_registry<TimestampKeySetEntry, TimestampKeySetHandle>(
      options_.max_timestamp_key_sets, registry_generation(activation_generation))),
  streams_(make_registry<StreamEntry, StreamHandle>(
      options_.max_streams, registry_generation(activation_generation))),
  descriptors_(make_registry<StreamDescriptorEntry, StreamDescriptorHandle>(
      options_.max_stream_descriptors, registry_generation(activation_generation))),
  bindings_(make_registry<BindingEntry, BindingHandle>(
      options_.max_bindings, registry_generation(activation_generation))),
  sequences_([this] {
      auto result = AcceptanceSequenceAllocator::Create(options_.max_streams);
      if (!result.has_value()) {
          throw std::runtime_error(std::string(result.error().message()));
      }
      return std::move(result.value());
  }()),
  payload_pool_([this] {
      auto result = PayloadPool::Create(options_.payload_block_bytes, options_.payload_block_count);
      if (!result.has_value()) {
          throw std::runtime_error(std::string(result.error().message()));
      }
      return std::move(result.value());
  }()),
  projections_(ProjectionLimits{128, 8, 1024U * 1024U, 8U * 1024U * 1024U}),
  state_(RuntimeState::Starting) {
    source_handles_.reserve(options_.max_sources);
    clock_handles_.reserve(options_.max_clocks);
    schema_handles_.reserve(options_.max_schemas);
    timestamp_key_set_handles_.reserve(options_.max_timestamp_key_sets);
    stream_handles_.reserve(options_.max_streams);
    descriptor_handles_.reserve(options_.max_stream_descriptors);
    binding_handles_.reserve(options_.max_bindings);
    secondary_failures_.reserve(kMaxSecondaryFailures);
    acquisition_controls_.resize(options_.acquisitions.size());
    acquisition_instances_.resize(options_.acquisitions.size());
    acquisition_contexts_.reserve(options_.acquisitions.size());
    for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
        auto context = std::make_unique<AcquisitionServiceContext>();
        context->state = this;
        context->plugin_index = index;
        acquisition_contexts_.push_back(std::move(context));
    }
    storage_context_ = std::make_unique<StorageServiceContext>();
    storage_context_->state = this;
    auto ingress = InitializeIngressClock();
    if (!ingress.has_value()) {
        throw std::runtime_error(std::string(ingress.error().message()));
    }
    projections_.SetRegistrationSink(
        [this](rodaq_stream_descriptor_handle descriptor, std::string_view projection_id) {
            OnProjectionRegistered(descriptor, projection_id);
        });
}

void ActiveRuntimeState::SetAcquisitionControl(std::size_t plugin_index, rodaq_interface control) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (plugin_index >= acquisition_controls_.size() || control.context == nullptr ||
        control.vtable == nullptr) {
        throw std::invalid_argument("Acquisition control interface is invalid");
    }
    acquisition_controls_[plugin_index] = control;
}

void ActiveRuntimeState::SetStorageControl(rodaq_interface control) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (control.context == nullptr || control.vtable == nullptr) {
        throw std::invalid_argument("Storage control interface is invalid");
    }
    storage_control_ = control;
}

Result<void> ActiveRuntimeState::InitializeIngressClock() {
    static constexpr std::string_view kNativeId = "rodaq/runtime/ingress";
    static constexpr std::string_view kOrigin = "monotonic_since_boot";

    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (ingress_clock_.Valid()) {
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::InvalidState, "Runtime ingress Clock is already created"));
        }
        auto inserted = clocks_.Insert(ClockEntry{
            copy_bytes(
                arena_->Resource(), reinterpret_cast<const std::uint8_t*>(kNativeId.data()),
                kNativeId.size()),
            next_clock_id_++,
            1,
            1'000'000'000,
            copy_string(arena_->Resource(), kOrigin),
            kRuntimeClockPluginIndex,
        });
        if (!inserted.has_value()) {
            return nonstd::make_unexpected(std::move(inserted.error()));
        }
        ingress_clock_ = inserted.value();
        clock_handles_.push_back(ingress_clock_);
    }

    auto correlation = ClockCorrelation::Establish(
        rodaq_clock_handle{ingress_clock_.Value()},
        []() -> std::int64_t {
            auto reading = capture_ingress_reading();
            if (!reading.has_value()) {
                throw std::runtime_error(std::string(reading.error().message()));
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
    if (!correlation.has_value()) {
        return nonstd::make_unexpected(std::move(correlation.error()));
    }
    ingress_clock_correlation_.emplace(std::move(correlation.value()));
    return {};
}

Result<rodaq_storage_capabilities> ActiveRuntimeState::StorageCapabilities() const {
    if (!storage_control_.has_value()) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidState, "Storage control interface is unavailable"));
    }
    const auto* table = static_cast<const rodaq_storage_control_v1*>(storage_control_->vtable);
    if (table->get_capabilities == nullptr) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Storage capabilities slot is missing"));
    }
    rodaq_storage_capabilities capabilities{};
    capabilities.struct_size = sizeof(capabilities);
    rodaq_error error{};
    const rodaq_status status =
        table->get_capabilities(storage_control_->context, &capabilities, &error);
    if (status != RODAQ_STATUS_OK) {
        auto converted = result_from_status(status, &error, "storage");
        return nonstd::make_unexpected(
            converted.has_value()
                ? runtime_error(ErrorCode::InternalError, "Storage capabilities failed")
                : std::move(converted.error()));
    }
    if (capabilities.struct_size < RODAQ_STORAGE_CAPABILITIES_MIN_STRUCT_SIZE) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Storage capabilities output is incomplete"));
    }
    return capabilities;
}

void ActiveRuntimeState::SetDescriptorProgressTimeout(std::chrono::nanoseconds timeout) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    descriptor_progress_timeout_ = timeout;
}

Result<ActiveRuntimeState::DescriptorReadiness> ActiveRuntimeState::EnsureDescriptorReady(
    StreamDescriptorHandle descriptor) {
    if (!storage_control_.has_value()) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidState, "Storage control interface is unavailable"));
    }
    const rodaq_interface control = storage_control_.value();
    const auto* table = static_cast<const rodaq_storage_control_v1*>(control.vtable);
    if (table->ensure_descriptor_ready == nullptr) {
        return nonstd::make_unexpected(runtime_error(
            ErrorCode::InvalidArgument, "Storage descriptor readiness slot is missing"));
    }

    std::shared_ptr<ActiveRuntimeState> state;
    try {
        state = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidState, "Runtime activation has no shared owner"));
    }
    auto result = std::make_shared<std::optional<DescriptorReadiness>>();
    auto result_error = std::make_shared<std::optional<Error>>();
    const auto descriptor_value = rodaq_stream_descriptor_handle{descriptor.Value()};
    auto outcome = QuarantineManager::Process().Run(
        [control, table, descriptor_value, result, result_error, state]() -> Result<void> {
            static_cast<void>(state);
            rodaq_descriptor_readiness_result readiness{};
            readiness.struct_size = sizeof(readiness);
            rodaq_error error{};
            const rodaq_status status = table->ensure_descriptor_ready(
                control.context, descriptor_value, &readiness, &error);
            if (status != RODAQ_STATUS_OK) {
                auto converted = result_from_status(status, &error, "storage");
                result_error->emplace(
                    converted.has_value()
                        ? runtime_error(
                              ErrorCode::InternalError, "Storage descriptor readiness failed")
                        : std::move(converted.error()));
                return {};
            }
            if (readiness.struct_size < RODAQ_DESCRIPTOR_READINESS_RESULT_MIN_STRUCT_SIZE) {
                result_error->emplace(runtime_error(
                    ErrorCode::InvalidArgument,
                    "Storage descriptor readiness output is incomplete"));
                return {};
            }
            if (readiness.state == RODAQ_DESCRIPTOR_UNSUPPORTED) {
                result->emplace(DescriptorReadiness::Unsupported);
            } else if (readiness.state == RODAQ_DESCRIPTOR_READY) {
                result->emplace(DescriptorReadiness::Ready);
            } else {
                result_error->emplace(
                    runtime_error(ErrorCode::StorageError, "Storage descriptor readiness failed"));
            }
            return {};
        },
        Clock::now() + descriptor_progress_timeout_, "Descriptor readiness");
    if (!outcome.has_value()) {
        return nonstd::make_unexpected(std::move(outcome.error()));
    }
    switch (outcome.value().status) {
        case SupervisedCallStatus::TimedOut:
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::Timeout, "Storage descriptor readiness timed out"));
        case SupervisedCallStatus::Failed:
            return nonstd::make_unexpected(std::move(outcome.value().error.value()));
        case SupervisedCallStatus::Completed:
            break;
    }
    if (result_error->has_value()) {
        return nonstd::make_unexpected(std::move(result_error->value()));
    }
    if (!result->has_value()) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InternalError, "Descriptor readiness produced no outcome"));
    }
    return result->value();
}

Result<void> ActiveRuntimeState::InitializePipeline(
    std::size_t effective_payload_limit, std::size_t maximum_payload_inflight_bytes,
    plugin::StoragePluginInstance* storage, std::chrono::nanoseconds progress_timeout) {
    if (pipeline_ != nullptr) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidState, "Runtime pipeline is already initialized"));
    }
    if (effective_payload_limit == 0 || options_.payload_block_bytes < effective_payload_limit) {
        return nonstd::make_unexpected(runtime_error(
            ErrorCode::InvalidArgument,
            "Configured payload block size is below the effective payload limit"));
    }
    if (options_.payload_block_count < options_.operation_capacity) {
        return nonstd::make_unexpected(runtime_error(
            ErrorCode::InvalidArgument, "Payload pool block count is below operation capacity"));
    }
    try {
        pipeline_ = std::make_unique<StoragePipeline>(
            options_.operation_capacity, options_.completion_capacity, effective_payload_limit,
            maximum_payload_inflight_bytes, storage, progress_timeout,
            [this](Error error) { RequestFailure(std::move(error)); },
            [this](std::size_t bytes) noexcept {
                if (!stats_.RecordPayloadReleased(bytes)) {
                    RequestFailure(runtime_error(
                        ErrorCode::InternalError, "Runtime inflight byte accounting underflowed"));
                }
            },
            [this]() noexcept {
                if (!stats_.RecordOperationCompleted()) {
                    RequestFailure(runtime_error(
                        ErrorCode::InternalError,
                        "Runtime inflight record accounting underflowed"));
                }
            },
            [this](
                const RuntimeRecord& record, rodaq_storage_operation operation,
                std::array<rodaq_timestamp_value, TimestampState::kMaxSlots>& values) {
                std::lock_guard<std::mutex> lock(control_mutex_);
                const BindingEntry* binding = bindings_.Find(record.Binding());
                if (binding == nullptr) {
                    return Result<rodaq_storage_submission>(nonstd::make_unexpected(
                        runtime_error(ErrorCode::InvalidState, "Record Binding is unavailable")));
                }
                const StreamDescriptorEntry* descriptor = descriptors_.Find(binding->descriptor);
                if (descriptor == nullptr) {
                    return Result<rodaq_storage_submission>(nonstd::make_unexpected(runtime_error(
                        ErrorCode::InvalidState, "Record Descriptor is unavailable")));
                }
                for (std::size_t slot = 0; slot < TimestampState::kMaxSlots; ++slot) {
                    values[slot] = {};
                    const auto reading = record.Timestamps().Value(slot);
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
                submission.stream = rodaq_stream_handle{binding->stream.Value()};
                submission.descriptor = rodaq_stream_descriptor_handle{binding->descriptor.Value()};
                submission.acceptance_sequence = record.AcceptanceSequence();
                if (record.Source().has_value()) {
                    submission.source_present = RODAQ_TRUE;
                    submission.source.value = record.Source()->Value();
                }
                if (record.SourceSequence().has_value()) {
                    submission.source_sequence_present = RODAQ_TRUE;
                    submission.source_sequence = *record.SourceSequence();
                }
                submission.timestamp_values = values.data();
                submission.timestamp_value_count =
                    static_cast<std::uint32_t>(descriptor->canonical_timestamp_keys.size());
                const PayloadView payload = record.RecordPayload().View();
                submission.payload = rodaq_bytes_view{payload.Data(), payload.Size()};
                return Result<rodaq_storage_submission>(submission);
            });
        auto started = pipeline_->Start();
        if (!started.has_value()) {
            pipeline_.reset();
            return nonstd::make_unexpected(std::move(started.error()));
        }
        return {};
    } catch (const std::exception& error) {
        return nonstd::make_unexpected(runtime_error(ErrorCode::InternalError, error.what()));
    }
}

Result<void> ActiveRuntimeState::RegisterAcquisitionServices(
    plugin::ServiceRegistry& registry, std::size_t plugin_index) {
    if (plugin_index >= acquisition_contexts_.size()) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Acquisition service index is invalid"));
    }
    auto result =
        registry.Register(RODAQ_RUNTIME_COMMON_SERVICE_ID, rodaq_interface{this, &kCommonVtable});
    if (!result.has_value()) {
        return result;
    }
    result = registry.Register(
        RODAQ_RUNTIME_CANONICAL_SERVICE_ID, rodaq_interface{this, &kCanonicalVtable});
    if (!result.has_value()) {
        return result;
    }
    result = registry.Register(RODAQ_RUNTIME_PROJECTION_SERVICE_ID, projections_.AbiInterface());
    if (!result.has_value()) {
        return result;
    }
    result = registry.Register(
        RODAQ_RUNTIME_ACQUISITION_SERVICE_ID, AcquisitionServiceInterface(plugin_index));
    return result;
}

Result<void> ActiveRuntimeState::RegisterStorageServices(plugin::ServiceRegistry& registry) {
    auto result =
        registry.Register(RODAQ_RUNTIME_COMMON_SERVICE_ID, rodaq_interface{this, &kCommonVtable});
    if (!result.has_value()) {
        return result;
    }
    result = registry.Register(
        RODAQ_RUNTIME_CANONICAL_SERVICE_ID, rodaq_interface{this, &kCanonicalVtable});
    if (!result.has_value()) {
        return result;
    }
    result = registry.Register(RODAQ_RUNTIME_PROJECTION_SERVICE_ID, projections_.AbiInterface());
    if (!result.has_value()) {
        return result;
    }
    if (!ingress_clock_correlation_.has_value()) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidState, "Runtime ingress Clock is unavailable"));
    }
    result = registry.Register(
        RODAQ_RUNTIME_CLOCK_CORRELATION_SERVICE_ID, ingress_clock_correlation_->AbiInterface());
    if (!result.has_value()) {
        return result;
    }
    result = registry.Register(RODAQ_RUNTIME_STORAGE_SERVICE_ID, StorageServiceInterface());
    return result;
}

rodaq_interface ActiveRuntimeState::AcquisitionServiceInterface(std::size_t plugin_index) noexcept {
    return rodaq_interface{
        acquisition_contexts_[plugin_index].get(),
        &kAcquisitionVtable,
    };
}

rodaq_interface ActiveRuntimeState::StorageServiceInterface() noexcept {
    return rodaq_interface{
        storage_context_.get(),
        &kStorageVtable,
    };
}

void ActiveRuntimeState::MarkRunning() noexcept {
    if (pipeline_ != nullptr) {
        pipeline_->OpenAdmission();
    }
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        gate_open_ = pipeline_ != nullptr;
        state_.store(RuntimeState::Running);
    }
    admission_condition_.notify_all();
}

void ActiveRuntimeState::MarkStopping() noexcept {
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        if (state_.load() != RuntimeState::Failed) {
            state_.store(RuntimeState::Stopping);
        }
        gate_open_ = false;
    }
    if (pipeline_ != nullptr) {
        pipeline_->CloseAdmission();
    }
    admission_condition_.notify_all();
}

void ActiveRuntimeState::MarkStopped() noexcept {
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        if (state_.load() != RuntimeState::Failed) {
            state_.store(RuntimeState::Stopped);
        }
        gate_open_ = false;
    }
    admission_condition_.notify_all();
}

void ActiveRuntimeState::MarkFailed() noexcept {
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        state_.store(RuntimeState::Failed);
        gate_open_ = false;
    }
    if (pipeline_ != nullptr) {
        pipeline_->CloseAdmission();
    }
    admission_condition_.notify_all();
}

bool ActiveRuntimeState::GateOpen() const noexcept {
    std::lock_guard<std::mutex> lock(admission_mutex_);
    return gate_open_;
}

bool ActiveRuntimeState::BeginSubmission() noexcept {
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        ++active_submission_calls_;
    }
    return true;
}

void ActiveRuntimeState::EndSubmission() noexcept {
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        --active_submission_calls_;
    }
    admission_condition_.notify_all();
}

bool ActiveRuntimeState::WaitForAdmissionDrain(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(admission_mutex_);
    return admission_condition_.wait_until(lock, deadline, [this] {
        return active_submission_calls_ == 0 && admission_inflight_ == 0;
    });
}

bool ActiveRuntimeState::WaitForSubmissions(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(admission_mutex_);
    return admission_condition_.wait_until(
        lock, deadline, [this] { return active_submission_calls_ == 0; });
}

void ActiveRuntimeState::RequestFailure(Error error) noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(failure_mutex_);
            if (!primary_failure_.has_value()) {
                primary_failure_.emplace(std::move(error));
            } else if (secondary_failures_.size() < kMaxSecondaryFailures) {
                secondary_failures_.push_back(std::move(error));
            } else {
                ++secondary_overflow_count_;
            }
        }
        MarkFailed();
    } catch (...) {
    }
}

Result<Error> ActiveRuntimeState::PrimaryFailure() const {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    if (!primary_failure_.has_value()) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::NotFound, "Runtime has no primary failure"));
    }
    return primary_failure_.value();
}

Result<std::vector<Error>> ActiveRuntimeState::SecondaryFailures() const {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    if (primary_failure_.has_value()) {
        return secondary_failures_;
    }
    return nonstd::make_unexpected(
        runtime_error(ErrorCode::InvalidState, "Runtime has no primary failure"));
}

std::uint64_t ActiveRuntimeState::SecondaryFailureOverflowCount() const noexcept {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return secondary_overflow_count_;
}

void ActiveRuntimeState::RecordPreRejected(bool gate_closed, bool timeout) noexcept {
    if (gate_closed) {
        stats_.RecordGateClosed();
    } else if (timeout) {
        stats_.RecordTimedOut();
    } else {
        stats_.RecordRejected();
    }
}

Result<rodaq_runtime_lifecycle_snapshot> ActiveRuntimeState::LifecycleSnapshot() const {
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
        state == RuntimeState::Running && GateOpen() ? RODAQ_TRUE : RODAQ_FALSE;
    snapshot.generation = activation_generation_;
    return snapshot;
}

Result<void> ActiveRuntimeState::RegisterSource(
    std::size_t plugin_index, const rodaq_source_registration& request, SourceHandle& handle) {
    static_cast<void>(plugin_index);
    if (!rodaq::plugin::is_valid_source_registration(request)) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Source registration is invalid"));
    }
    const auto* source_id = request.source_id.data;
    const std::size_t source_id_size = static_cast<std::size_t>(request.source_id.size);

    std::lock_guard<std::mutex> lock(control_mutex_);
    const RuntimeState current_state = state_.load();
    if (current_state != RuntimeState::Starting && current_state != RuntimeState::Running) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidState, "Source registration is not allowed"));
    }
    for (SourceHandle existing : source_handles_) {
        const SourceEntry* entry = sources_.Find(existing);
        if (entry != nullptr && bytes_equal(entry->source_id, source_id, source_id_size)) {
            handle = existing;
            return {};
        }
    }

    auto availability = std::make_unique<std::atomic<std::uint32_t>>(RODAQ_AVAILABILITY_AVAILABLE);
    auto inserted = sources_.Insert(SourceEntry{
        copy_bytes(arena_->Resource(), source_id, source_id_size), std::move(availability)});
    if (!inserted.has_value()) {
        return nonstd::make_unexpected(std::move(inserted.error()));
    }
    source_handles_.push_back(inserted.value());
    handle = inserted.value();
    return {};
}

Result<void> ActiveRuntimeState::RegisterClock(
    std::size_t plugin_index, const rodaq_clock_registration& request, ClockHandle& handle) {
    if (!rodaq::plugin::is_valid_clock_registration(request) ||
        plugin_index >= options_.acquisitions.size()) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Clock registration is invalid"));
    }
    std::string_view origin;
    static_cast<void>(parse_abi_string(request.origin, &origin));
    const std::size_t native_size = static_cast<std::size_t>(request.native_id.size);

    std::lock_guard<std::mutex> lock(control_mutex_);
    const RuntimeState current_state = state_.load();
    if (current_state != RuntimeState::Starting && current_state != RuntimeState::Running) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidState, "Clock registration is not allowed"));
    }
    for (ClockHandle existing : clock_handles_) {
        const ClockEntry* entry = clocks_.Find(existing);
        if (entry == nullptr || entry->plugin_index != plugin_index ||
            !bytes_equal(entry->native_id, request.native_id.data, native_size)) {
            continue;
        }
        if (entry->resolution_numerator != request.resolution_numerator ||
            entry->resolution_denominator != request.resolution_denominator ||
            !strings_equal(entry->origin, origin)) {
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::Conflict, "Clock registration facts conflict"));
        }
        handle = existing;
        return {};
    }

    const std::uint64_t clock_id = next_clock_id_++;
    auto inserted = clocks_.Insert(ClockEntry{
        copy_bytes(arena_->Resource(), request.native_id.data, native_size),
        clock_id,
        request.resolution_numerator,
        request.resolution_denominator,
        copy_string(arena_->Resource(), origin),
        plugin_index,
    });
    if (!inserted.has_value()) {
        return nonstd::make_unexpected(std::move(inserted.error()));
    }
    clock_handles_.push_back(inserted.value());
    handle = inserted.value();
    return {};
}

Result<void> ActiveRuntimeState::RegisterSchema(
    const rodaq_schema_registration& request, SchemaHandle& handle) {
    if (!rodaq::plugin::is_valid_schema_registration(request)) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Schema registration is invalid"));
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
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidState, "Schema registration is not allowed"));
    }
    for (SchemaHandle existing : schema_handles_) {
        const SchemaEntry* entry = schemas_.Find(existing);
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
    auto inserted = schemas_.Insert(SchemaEntry{
        schema_id,
        copy_string(arena_->Resource(), format),
        request.type_identifier_present == RODAQ_TRUE,
        copy_string(arena_->Resource(), type_identifier),
        copy_bytes(arena_->Resource(), request.representation.data, representation_size),
    });
    if (!inserted.has_value()) {
        return nonstd::make_unexpected(std::move(inserted.error()));
    }
    schema_handles_.push_back(inserted.value());
    handle = inserted.value();
    return {};
}

Result<void> ActiveRuntimeState::RegisterStream(
    std::size_t plugin_index, const rodaq_stream_registration& request, StreamHandle& handle) {
    if (!rodaq::plugin::is_valid_stream_registration(request) ||
        plugin_index >= options_.acquisitions.size()) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Stream registration is invalid"));
    }
    const std::size_t native_size = static_cast<std::size_t>(request.native_id.size);

    std::lock_guard<std::mutex> lock(control_mutex_);
    const RuntimeState current_state = state_.load();
    if (current_state != RuntimeState::Starting && current_state != RuntimeState::Running) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidState, "Stream registration is not allowed"));
    }
    for (StreamHandle existing : stream_handles_) {
        const StreamEntry* entry = streams_.Find(existing);
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
    auto inserted = streams_.Insert(StreamEntry{
        next_stream_id_++,
        plugin_index,
        copy_bytes(arena_->Resource(), request.native_id.data, native_size),
        std::move(availability),
        std::move(current_binding),
        std::move(admission_mutex),
        std::move(binding_transition_mutex),
        std::move(binding_transition_condition),
        false,
    });
    if (!inserted.has_value()) {
        return nonstd::make_unexpected(std::move(inserted.error()));
    }
    stream_handles_.push_back(inserted.value());
    handle = inserted.value();
    return {};
}

Result<void> ActiveRuntimeState::PublishStreamDescriptor(
    std::size_t plugin_index, const rodaq_stream_descriptor_registration& request,
    StreamDescriptorHandle& handle) {
    if (!rodaq::plugin::is_valid_stream_descriptor_registration(request) ||
        request.timestamp_key_count + 1U > TimestampState::kMaxSlots ||
        plugin_index >= options_.acquisitions.size()) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "StreamDescriptor publication is invalid"));
    }
    std::string_view name;
    std::string_view payload_encoding;
    parse_abi_string(request.name, &name);
    parse_abi_string(request.payload_encoding, &payload_encoding);

    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        const RuntimeState current_state = state_.load();
        if (current_state != RuntimeState::Starting && current_state != RuntimeState::Running) {
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::InvalidState, "Descriptor publication is not allowed"));
        }
        const StreamHandle stream_handle = StreamHandle::FromRaw(request.stream.value);
        StreamEntry* stream = streams_.Find(stream_handle);
        if (stream == nullptr) {
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::NotFound, "Descriptor Stream is unknown"));
        }
        if (stream->plugin_index != plugin_index) {
            return nonstd::make_unexpected(runtime_error(
                ErrorCode::InvalidState, "Descriptor Stream belongs to another Plugin"));
        }

        std::pmr::vector<TimestampKeyRecord> keys(arena_->Resource());
        std::pmr::vector<rodaq_timestamp_key> abi_keys(arena_->Resource());
        std::pmr::vector<rodaq_timestamp_key> transport_abi_keys(arena_->Resource());
        keys.reserve(request.timestamp_key_count + 1U);
        abi_keys.reserve(request.timestamp_key_count + 1U);
        transport_abi_keys.reserve(request.timestamp_key_count);
        keys.push_back(TimestampKeyRecord{
            copy_string(arena_->Resource(), kIngressTimestampRole), ingress_clock_});
        for (std::uint32_t index = 0; index < request.timestamp_key_count; ++index) {
            const rodaq_timestamp_key& key = request.timestamp_keys[index];
            std::string_view role;
            if (!parse_abi_string(key.role, &role)) {
                return nonstd::make_unexpected(
                    runtime_error(ErrorCode::InvalidArgument, "Timestamp role is invalid"));
            }
            if (role == kIngressTimestampRole) {
                return nonstd::make_unexpected(runtime_error(
                    ErrorCode::InvalidArgument,
                    "Acquisition Plugins must not publish the Runtime ingress timestamp role"));
            }
            const ClockHandle clock = ClockHandle::FromRaw(key.clock.value);
            if (clocks_.Find(clock) == nullptr) {
                return nonstd::make_unexpected(
                    runtime_error(ErrorCode::NotFound, "Timestamp Clock is unknown"));
            }
            keys.push_back(TimestampKeyRecord{copy_string(arena_->Resource(), role), clock});
            transport_abi_keys.push_back(rodaq_timestamp_key{
                rodaq_string_view{keys.back().role.data(), keys.back().role.size()},
                rodaq_clock_handle{clock.Value()},
            });
        }

        TimestampKeySetHandle key_set_handle;
        bool found_key_set = false;
        for (TimestampKeySetHandle existing : timestamp_key_set_handles_) {
            const TimestampKeySetEntry* entry = timestamp_key_sets_.Find(existing);
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
            auto inserted = timestamp_key_sets_.Insert(std::move(entry));
            if (!inserted.has_value()) {
                return nonstd::make_unexpected(std::move(inserted.error()));
            }
            key_set_handle = inserted.value();
            timestamp_key_set_handles_.push_back(key_set_handle);
        }
        TimestampKeySetEntry* key_set = timestamp_key_sets_.Find(key_set_handle);
        if (key_set == nullptr) {
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::InternalError, "TimestampKeySet publication failed"));
        }
        key_set_id = key_set->timestamp_key_set_id;
        if (key_set->abi_keys.size() != key_set->keys.size()) {
            key_set->abi_keys.clear();
            for (const TimestampKeyRecord& key : key_set->keys) {
                key_set->abi_keys.push_back(rodaq_timestamp_key{
                    rodaq_string_view{key.role.data(), key.role.size()},
                    rodaq_clock_handle{key.clock.Value()},
                });
            }
        }

        StreamDescriptorEntry descriptor_entry{
            next_descriptor_id_++,
            stream_handle,
            request.schema_present == RODAQ_TRUE,
            SchemaHandle::FromRaw(request.schema.value),
            0,
            copy_string(arena_->Resource(), name),
            copy_string(arena_->Resource(), payload_encoding),
            key_set_id,
            std::pmr::vector<rodaq_timestamp_key>(arena_->Resource()),
            std::move(transport_abi_keys),
            plugin_index,
        };
        if (descriptor_entry.schema_present) {
            const SchemaEntry* schema = schemas_.Find(descriptor_entry.schema);
            if (schema == nullptr) {
                return nonstd::make_unexpected(
                    runtime_error(ErrorCode::NotFound, "Descriptor Schema is unknown"));
            }
            descriptor_entry.schema_id = schema->schema_id;
        }
        descriptor_entry.canonical_timestamp_keys.assign(
            key_set->abi_keys.begin(), key_set->abi_keys.end());

        auto descriptor_inserted = descriptors_.Insert(std::move(descriptor_entry));
        if (!descriptor_inserted.has_value()) {
            return nonstd::make_unexpected(std::move(descriptor_inserted.error()));
        }
        handle = descriptor_inserted.value();
        descriptor_handles_.push_back(handle);
    }

    auto reconciled = ReconcileDescriptor(handle);
    if (!reconciled.has_value()) {
        return nonstd::make_unexpected(std::move(reconciled.error()));
    }
    return {};
}

Result<bool> ActiveRuntimeState::ReconcileDescriptor(StreamDescriptorHandle descriptor) {
    while (true) {
        StreamHandle stream;
        std::size_t plugin_index = 0;
        rodaq_interface control{};
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamDescriptorEntry* entry = descriptors_.Find(descriptor);
            if (entry == nullptr) {
                return nonstd::make_unexpected(
                    runtime_error(ErrorCode::NotFound, "Descriptor is not published"));
            }
            if (entry->lifecycle == DescriptorLifecycle::Active) {
                return true;
            }
            if (entry->lifecycle == DescriptorLifecycle::Failed ||
                entry->lifecycle == DescriptorLifecycle::Superseded) {
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
                return nonstd::make_unexpected(runtime_error(
                    ErrorCode::InvalidState, "Acquisition control interface is unavailable"));
            }
            control = acquisition_controls_[plugin_index].value();
        }

        auto readiness = EnsureDescriptorReady(descriptor);
        bool retry = false;
        if (!readiness.has_value()) {
            {
                std::lock_guard<std::mutex> lock(control_mutex_);
                StreamDescriptorEntry* entry = descriptors_.Find(descriptor);
                if (entry != nullptr) {
                    entry->reconciliation_running = false;
                    entry->lifecycle = DescriptorLifecycle::Failed;
                    retry = entry->reconciliation_retry_requested;
                    entry->reconciliation_retry_requested = false;
                }
            }
            RequestFailure(readiness.error());
            return nonstd::make_unexpected(std::move(readiness.error()));
        }
        if (readiness.value() != DescriptorReadiness::Ready) {
            {
                std::lock_guard<std::mutex> lock(control_mutex_);
                StreamDescriptorEntry* entry = descriptors_.Find(descriptor);
                if (entry != nullptr) {
                    entry->reconciliation_running = false;
                    entry->lifecycle = DescriptorLifecycle::Unsupported;
                    retry = entry->reconciliation_retry_requested;
                    entry->reconciliation_retry_requested = false;
                    StreamEntry* stream_entry = streams_.Find(entry->stream);
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
            StreamDescriptorEntry* entry = descriptors_.Find(descriptor);
            if (entry != nullptr) {
                entry->lifecycle = DescriptorLifecycle::Ready;
            }
        }
        auto activated = ActivateDescriptorBinding(descriptor, stream, plugin_index, control);
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamDescriptorEntry* entry = descriptors_.Find(descriptor);
            if (entry != nullptr) {
                entry->reconciliation_running = false;
                entry->reconciliation_retry_requested = false;
            }
        }
        if (!activated.has_value()) {
            return nonstd::make_unexpected(std::move(activated.error()));
        }
        return true;
    }
}

Result<void> ActiveRuntimeState::ActivateDescriptorBinding(
    StreamDescriptorHandle descriptor, StreamHandle stream_handle, std::size_t plugin_index,
    rodaq_interface control) {
    StreamEntry* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        stream = streams_.Find(stream_handle);
        if (stream == nullptr) {
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::NotFound, "Descriptor Stream is unavailable"));
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
            }
            stream->binding_transition_condition->notify_one();
        }
    } transition_reset{stream};

    BindingHandle candidate;
    BindingHandle old_binding;
    std::uint64_t old_binding_value = 0;
    rodaq_stream_descriptor_handle old_descriptor{};
    rodaq_binding_transition transition{};
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        StreamDescriptorEntry* descriptor_entry = descriptors_.Find(descriptor);
        StreamEntry* current_stream = streams_.Find(stream_handle);
        if (descriptor_entry == nullptr || current_stream == nullptr ||
            descriptor_entry->lifecycle != DescriptorLifecycle::Ready) {
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::InvalidState, "Descriptor is no longer ready"));
        }
        old_binding_value = current_stream->current_binding->load(std::memory_order_acquire);
        if (old_binding_value != 0) {
            BindingEntry* current_binding =
                bindings_.Find(BindingHandle::FromRaw(old_binding_value));
            StreamDescriptorEntry* current_descriptor =
                current_binding == nullptr ? nullptr
                                           : descriptors_.Find(current_binding->descriptor);
            if (current_descriptor != nullptr &&
                current_descriptor->stream_descriptor_id > descriptor_entry->stream_descriptor_id) {
                descriptor_entry->lifecycle = DescriptorLifecycle::Superseded;
                descriptor_entry->reconciliation_running = false;
                return {};
            }
            old_binding = BindingHandle::FromRaw(old_binding_value);
            if (current_binding != nullptr) {
                old_descriptor =
                    rodaq_stream_descriptor_handle{current_binding->descriptor.Value()};
            }
        }
        auto created = CreateBindingLocked(
            plugin_index, stream_handle, descriptor, BindingLifecycle::Prepared);
        if (!created.has_value()) {
            descriptor_entry->lifecycle = DescriptorLifecycle::Failed;
            return nonstd::make_unexpected(std::move(created.error()));
        }
        candidate = created.value();
        descriptor_entry->lifecycle = DescriptorLifecycle::BindingPrepared;
        transition = rodaq_binding_transition{next_transition_id_++};
    }

    const auto* table = static_cast<const rodaq_acquisition_control_v1*>(control.vtable);
    rodaq_error error{};
    const rodaq_status prepare_status = table->prepare_binding_switch(
        control.context, transition, rodaq_stream_handle{stream_handle.Value()}, old_descriptor,
        rodaq_stream_descriptor_handle{descriptor.Value()}, &error);
    if (prepare_status != RODAQ_STATUS_OK) {
        bool resume_old = false;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamEntry* current_stream = streams_.Find(stream_handle);
            resume_old = old_binding.Valid() && current_stream != nullptr &&
                         current_stream->current_binding->load(std::memory_order_acquire) ==
                             old_binding_value &&
                         current_stream->availability->load() == RODAQ_AVAILABILITY_AVAILABLE;
        }
        const rodaq_status abort_status = table->abort_binding_switch(
            control.context, transition,
            resume_old ? RODAQ_BINDING_ABORT_RESUME_OLD : RODAQ_BINDING_ABORT_KEEP_PAUSED, nullptr);
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            BindingEntry* prepared = bindings_.Find(candidate);
            if (prepared != nullptr) {
                prepared->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
            }
            StreamDescriptorEntry* descriptor_entry = descriptors_.Find(descriptor);
            if (descriptor_entry != nullptr) {
                descriptor_entry->lifecycle = DescriptorLifecycle::Failed;
            }
            StreamEntry* current_stream = streams_.Find(stream_handle);
            if (!resume_old && current_stream != nullptr) {
                current_stream->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
            }
        }
        if (abort_status != RODAQ_STATUS_OK) {
            RequestFailure(runtime_error(
                ErrorCode::InternalError, "Binding abort after prepare failure also failed"));
        }
        auto converted = result_from_status(prepare_status, &error, "acquisition");
        return converted.has_value() ? nonstd::make_unexpected(runtime_error(
                                           ErrorCode::Unavailable, "Binding prepare failed"))
                                     : nonstd::make_unexpected(std::move(converted.error()));
    }

    {
        std::lock_guard<std::mutex> stream_lock(*stream->admission_mutex);
        std::lock_guard<std::mutex> control_lock(control_mutex_);
        StreamDescriptorEntry* descriptor_entry = descriptors_.Find(descriptor);
        BindingEntry* prepared = bindings_.Find(candidate);
        StreamEntry* current_stream = streams_.Find(stream_handle);
        BindingEntry* old = old_binding.Valid() ? bindings_.Find(old_binding) : nullptr;
        if (descriptor_entry == nullptr || prepared == nullptr || current_stream == nullptr ||
            descriptor_entry->lifecycle != DescriptorLifecycle::BindingPrepared ||
            current_stream->current_binding->load(std::memory_order_acquire) != old_binding_value ||
            (old_binding.Valid() && old == nullptr)) {
            if (prepared != nullptr) {
                prepared->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
            }
            if (descriptor_entry != nullptr) {
                descriptor_entry->lifecycle = DescriptorLifecycle::Failed;
            }
            return nonstd::make_unexpected(
                runtime_error(ErrorCode::InvalidState, "Binding transition became stale"));
        }
        prepared->lifecycle->store(
            static_cast<std::uint32_t>(BindingLifecycle::Active), std::memory_order_release);
        if (old != nullptr) {
            old->lifecycle->store(
                static_cast<std::uint32_t>(BindingLifecycle::Draining), std::memory_order_release);
        }
        current_stream->current_binding->store(candidate.Value(), std::memory_order_release);
        current_stream->availability->store(RODAQ_AVAILABILITY_AVAILABLE);
        descriptor_entry->lifecycle = DescriptorLifecycle::Active;
    }

    const rodaq_status commit_status =
        table->commit_binding_switch(control.context, transition, &error);
    if (commit_status != RODAQ_STATUS_OK) {
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamEntry* current_stream = streams_.Find(stream_handle);
            if (current_stream != nullptr) {
                current_stream->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
            }
        }
        RequestFailure(runtime_error(
            ErrorCode::Unavailable, "Binding commit failed after Runtime publication"));
        auto converted = result_from_status(commit_status, &error, "acquisition");
        return converted.has_value() ? nonstd::make_unexpected(runtime_error(
                                           ErrorCode::Unavailable, "Binding commit failed"))
                                     : nonstd::make_unexpected(std::move(converted.error()));
    }
    if (old_binding.Valid()) {
        std::lock_guard<std::mutex> lock(control_mutex_);
        BindingEntry* old = bindings_.Find(old_binding);
        if (old != nullptr && old->admission_inflight->load() == 0) {
            old->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
        }
    }
    return {};
}

void ActiveRuntimeState::OnProjectionRegistered(
    rodaq_stream_descriptor_handle descriptor_value, std::string_view) noexcept {
    try {
        const StreamDescriptorHandle descriptor =
            StreamDescriptorHandle::FromRaw(descriptor_value.value);
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            StreamDescriptorEntry* entry = descriptors_.Find(descriptor);
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
        }
        auto result = ReconcileDescriptor(descriptor);
        if (!result.has_value()) {
            RequestFailure(result.error());
        }
    } catch (const std::exception& error) {
        RequestFailure(runtime_error(ErrorCode::InternalError, error.what()));
    } catch (...) {
        RequestFailure(runtime_error(ErrorCode::InternalError, "Descriptor reconciliation failed"));
    }
}

Result<BindingHandle> ActiveRuntimeState::CreateBindingLocked(
    std::size_t plugin_index, StreamHandle stream, StreamDescriptorHandle descriptor,
    BindingLifecycle lifecycle) {
    auto entry = BindingEntry{
        stream,
        descriptor,
        plugin_index,
        std::make_unique<std::atomic<std::uint32_t>>(static_cast<std::uint32_t>(lifecycle)),
        std::make_unique<std::atomic<std::uint64_t>>(0),
    };
    auto inserted = bindings_.Insert(std::move(entry));
    if (!inserted.has_value()) {
        return nonstd::make_unexpected(std::move(inserted.error()));
    }
    binding_handles_.push_back(inserted.value());
    return inserted.value();
}

void ActiveRuntimeState::RetireDrainingBinding(BindingHandle binding) noexcept {
    std::lock_guard<std::mutex> lock(control_mutex_);
    BindingEntry* entry = bindings_.Find(binding);
    if (entry != nullptr &&
        entry->lifecycle->load() == static_cast<std::uint32_t>(BindingLifecycle::Draining) &&
        entry->admission_inflight->load() == 0) {
        entry->lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Retired));
    }
}

Result<void> ActiveRuntimeState::ReportSourceAvailability(
    SourceHandle source, rodaq_availability_state availability) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    SourceEntry* entry = sources_.Find(source);
    if (entry == nullptr) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::NotFound, "Source handle is unknown"));
    }
    if (availability != RODAQ_AVAILABILITY_AVAILABLE &&
        availability != RODAQ_AVAILABILITY_UNAVAILABLE) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Source availability is invalid"));
    }
    entry->availability->store(availability);
    return {};
}

Result<void> ActiveRuntimeState::ReportStreamAvailability(
    StreamHandle stream, rodaq_availability_state availability) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    StreamEntry* entry = streams_.Find(stream);
    if (entry == nullptr) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::NotFound, "Stream handle is unknown"));
    }
    if (availability != RODAQ_AVAILABILITY_AVAILABLE &&
        availability != RODAQ_AVAILABILITY_UNAVAILABLE) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::InvalidArgument, "Stream availability is invalid"));
    }
    entry->availability->store(availability);
    return {};
}

Result<void> ActiveRuntimeState::GetSource(SourceHandle handle, rodaq_source_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const SourceEntry* entry = sources_.Find(handle);
    if (entry == nullptr) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::NotFound, "Source handle was not found"));
    }
    view.struct_size = sizeof(view);
    view.handle = rodaq_source_handle{handle.Value()};
    view.source_id = rodaq_bytes_view{entry->source_id.data(), entry->source_id.size()};
    return {};
}

Result<void> ActiveRuntimeState::GetClock(ClockHandle handle, rodaq_clock_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const ClockEntry* entry = clocks_.Find(handle);
    if (entry == nullptr) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::NotFound, "Clock handle was not found"));
    }
    view.struct_size = sizeof(view);
    view.handle = rodaq_clock_handle{handle.Value()};
    view.clock_id = entry->clock_id;
    view.resolution_numerator = entry->resolution_numerator;
    view.resolution_denominator = entry->resolution_denominator;
    view.origin = rodaq_string_view{entry->origin.data(), entry->origin.size()};
    return {};
}

Result<void> ActiveRuntimeState::GetSchema(SchemaHandle handle, rodaq_schema_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const SchemaEntry* entry = schemas_.Find(handle);
    if (entry == nullptr) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::NotFound, "Schema handle was not found"));
    }
    view.struct_size = sizeof(view);
    view.type_identifier_present = entry->type_identifier_present ? RODAQ_TRUE : RODAQ_FALSE;
    view.handle = rodaq_schema_handle{handle.Value()};
    view.schema_id = entry->schema_id;
    view.format = rodaq_string_view{entry->format.data(), entry->format.size()};
    view.type_identifier =
        rodaq_string_view{entry->type_identifier.data(), entry->type_identifier.size()};
    view.representation =
        rodaq_bytes_view{entry->representation.data(), entry->representation.size()};
    return {};
}

Result<void> ActiveRuntimeState::GetTimestampKeySet(
    std::uint64_t id, rodaq_timestamp_key_set_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    for (TimestampKeySetHandle handle : timestamp_key_set_handles_) {
        const TimestampKeySetEntry* entry = timestamp_key_sets_.Find(handle);
        if (entry == nullptr || entry->timestamp_key_set_id != id) {
            continue;
        }
        view.struct_size = sizeof(view);
        view.timestamp_key_set_id = id;
        view.keys = entry->abi_keys.data();
        view.key_count = static_cast<std::uint32_t>(entry->abi_keys.size());
        return {};
    }
    return nonstd::make_unexpected(
        runtime_error(ErrorCode::NotFound, "TimestampKeySet was not found"));
}

Result<void> ActiveRuntimeState::GetStream(StreamHandle handle, rodaq_stream_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const StreamEntry* entry = streams_.Find(handle);
    if (entry == nullptr) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::NotFound, "Stream handle was not found"));
    }
    view.struct_size = sizeof(view);
    view.handle = rodaq_stream_handle{handle.Value()};
    view.stream_id = entry->stream_id;
    return {};
}

Result<void> ActiveRuntimeState::GetStreamDescriptor(
    StreamDescriptorHandle handle, rodaq_stream_descriptor_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const StreamDescriptorEntry* entry = descriptors_.Find(handle);
    if (entry == nullptr) {
        return nonstd::make_unexpected(
            runtime_error(ErrorCode::NotFound, "StreamDescriptor handle was not found"));
    }
    view.struct_size = sizeof(view);
    view.schema_present = entry->schema_present ? RODAQ_TRUE : RODAQ_FALSE;
    view.handle = rodaq_stream_descriptor_handle{handle.Value()};
    view.stream = rodaq_stream_handle{entry->stream.Value()};
    view.schema = rodaq_schema_handle{entry->schema.Value()};
    view.stream_descriptor_id = entry->stream_descriptor_id;
    const StreamEntry* stream = streams_.Find(entry->stream);
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
        runtime_state.BeginSubmission();
    }
    ~SubmissionCallGuard() { state_->EndSubmission(); }

    ActiveRuntimeState* state_;
};

rodaq_acquisition_submit_result pre_rejected(
    ActiveRuntimeState& state, rodaq_admission_rejection_reason reason,
    rodaq_payload_disposition disposition, bool gate_closed = false,
    bool timeout = false) noexcept {
    state.RecordPreRejected(gate_closed, timeout);
    return submit_result(RODAQ_ADMISSION_PRE_REJECTED, reason, disposition);
}

}  // namespace

Result<rodaq_acquisition_submit_result> ActiveRuntimeState::Submit(
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
    if (state_.load() != RuntimeState::Running || !GateOpen() || pipeline_ == nullptr) {
        return pre_rejected(*this, RODAQ_REJECTION_RUNTIME_NOT_RUNNING, disposition, true);
    }

    const StreamHandle stream_handle = StreamHandle::FromRaw(submission.stream.value);
    const StreamDescriptorHandle descriptor_handle =
        StreamDescriptorHandle::FromRaw(submission.descriptor.value);
    StreamEntry* stream = streams_.Find(stream_handle);
    const StreamDescriptorEntry* descriptor = descriptors_.Find(descriptor_handle);
    if (stream == nullptr || descriptor == nullptr) {
        return pre_rejected(*this, RODAQ_REJECTION_STALE_HANDLE, disposition);
    }
    if (stream->plugin_index != plugin_index || descriptor->plugin_index != plugin_index) {
        return pre_rejected(*this, RODAQ_REJECTION_STALE_HANDLE, disposition);
    }
    if (stream->availability->load() != RODAQ_AVAILABILITY_AVAILABLE) {
        return pre_rejected(*this, RODAQ_REJECTION_STREAM_UNAVAILABLE, disposition);
    }
    if (descriptor->transport_timestamp_keys.size() != submission.timestamp_value_count) {
        return pre_rejected(*this, RODAQ_REJECTION_INVALID_INPUT, disposition);
    }

    std::optional<SourceHandle> source;
    std::optional<std::uint64_t> source_sequence;
    if (submission.source_present == RODAQ_TRUE) {
        source = SourceHandle::FromRaw(submission.source.value);
        const SourceEntry* source_entry = sources_.Find(*source);
        if (source_entry == nullptr) {
            return pre_rejected(*this, RODAQ_REJECTION_STALE_HANDLE, disposition);
        }
        if (source_entry->availability->load() != RODAQ_AVAILABILITY_AVAILABLE) {
            return pre_rejected(*this, RODAQ_REJECTION_SOURCE_UNAVAILABLE, disposition);
        }
    }
    if (submission.source_sequence_present == RODAQ_TRUE) {
        source_sequence = submission.source_sequence;
    }

    const std::size_t payload_size = static_cast<std::size_t>(submission.payload.size);
    if (payload_size > options_.max_single_payload_bytes) {
        return pre_rejected(*this, RODAQ_REJECTION_PAYLOAD_TOO_LARGE, disposition);
    }
    if (submission.payload.data == nullptr && payload_size != 0) {
        return pre_rejected(*this, RODAQ_REJECTION_PAYLOAD_INVALID, disposition);
    }

    const BindingHandle current_binding =
        BindingHandle::FromRaw(stream->current_binding->load(std::memory_order_acquire));
    BindingEntry* binding = current_binding.Valid() ? bindings_.Find(current_binding) : nullptr;
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
    if (!sequences_.CanAllocate(stream_handle)) {
        stream->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
        return pre_rejected(*this, RODAQ_REJECTION_SEQUENCE_EXHAUSTED, disposition);
    }

    const auto deadline = Clock::now() + options_.submit_wait_timeout;
    auto acquired = pipeline_->AcquirePreGate(payload_size, options_.admission_mode, deadline);
    if (!acquired.has_value()) {
        return nonstd::make_unexpected(std::move(acquired.error()));
    }
    switch (acquired.value().outcome) {
        case PreGateAcquireOutcome::GateClosed:
            return pre_rejected(*this, RODAQ_REJECTION_GATE_CLOSED, disposition, true);
        case PreGateAcquireOutcome::Timeout:
            return pre_rejected(*this, RODAQ_REJECTION_WAIT_TIMEOUT, disposition, false, true);
        case PreGateAcquireOutcome::ResourceUnavailable:
            return pre_rejected(*this, RODAQ_REJECTION_RESOURCE_UNAVAILABLE, disposition);
        case PreGateAcquireOutcome::Acquired:
            break;
    }
    PipelineReservation reservation = std::move(acquired.value().reservation);

    const PayloadView payload_view(submission.payload.data, payload_size);
    Payload payload;
    if (payload_mode == RODAQ_PAYLOAD_COPY) {
        auto copied = payload_pool_.Acquire(payload_size, payload_view);
        if (!copied.has_value()) {
            return pre_rejected(
                *this,
                copied.error().code() == ErrorCode::ResourceExhausted
                    ? RODAQ_REJECTION_RESOURCE_UNAVAILABLE
                    : RODAQ_REJECTION_PAYLOAD_INVALID,
                disposition);
        }
        payload = std::move(copied.value());
    } else if (payload_mode == RODAQ_PAYLOAD_ADOPT) {
        auto adopted = Payload::Adopt(
            payload_view, submission.payload.owner_context, submission.payload.release);
        if (!adopted.has_value()) {
            return pre_rejected(*this, RODAQ_REJECTION_PAYLOAD_INVALID, disposition);
        }
        payload = std::move(adopted.value());
        disposition = RODAQ_PAYLOAD_DISPOSITION_RUNTIME_RESPONSIBLE;
    } else {
        auto retained = Payload::Retain(
            payload_view, submission.payload.owner_context, submission.payload.release);
        if (!retained.has_value()) {
            return pre_rejected(*this, RODAQ_REJECTION_PAYLOAD_INVALID, disposition);
        }
        payload = std::move(retained.value());
        disposition = RODAQ_PAYLOAD_DISPOSITION_RUNTIME_RESPONSIBLE;
    }

    TimestampState timestamps;
    auto ingress_reading = capture_ingress_reading();
    if (!ingress_reading.has_value()) {
        RequestFailure(ingress_reading.error());
        return nonstd::make_unexpected(std::move(ingress_reading.error()));
    }
    auto ingress_set = timestamps.Set(0, ingress_reading.value());
    if (!ingress_set.has_value()) {
        RequestFailure(ingress_set.error());
        return nonstd::make_unexpected(std::move(ingress_set.error()));
    }
    for (std::size_t slot = 0; slot < descriptor->transport_timestamp_keys.size(); ++slot) {
        const rodaq_timestamp_value& value = submission.timestamp_values[slot];
        if (value.present == RODAQ_TRUE) {
            const auto set = timestamps.Set(slot + 1U, value.reading);
            if (!set.has_value()) {
                return pre_rejected(*this, RODAQ_REJECTION_INVALID_INPUT, disposition);
            }
        }
    }

    bool admission_failure = false;
    bool retire_binding = false;
    std::optional<Error> deferred_failure;
    std::optional<rodaq_acquisition_submit_result> deferred_result;
    {
        std::lock_guard<std::mutex> stream_lock(*stream->admission_mutex);
        std::lock_guard<std::mutex> gate_lock(admission_mutex_);
        if (state_.load() != RuntimeState::Running || !gate_open_) {
            return pre_rejected(*this, RODAQ_REJECTION_GATE_CLOSED, disposition, true);
        }
        if (!pipeline_->AdmissionOpen()) {
            admission_failure = true;
        }
        const BindingHandle final_binding =
            BindingHandle::FromRaw(stream->current_binding->load(std::memory_order_acquire));
        BindingEntry* final_binding_entry =
            final_binding.Valid() ? bindings_.Find(final_binding) : nullptr;
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
            const SourceEntry* source_entry = sources_.Find(*source);
            if (source_entry == nullptr ||
                source_entry->availability->load() != RODAQ_AVAILABILITY_AVAILABLE) {
                return pre_rejected(*this, RODAQ_REJECTION_SOURCE_UNAVAILABLE, disposition);
            }
        }
        if (!sequences_.CanAllocate(stream_handle)) {
            stream->availability->store(RODAQ_AVAILABILITY_UNAVAILABLE);
            return pre_rejected(*this, RODAQ_REJECTION_SEQUENCE_EXHAUSTED, disposition);
        }
        if (admission_failure) {
            deferred_failure.emplace(runtime_error(
                ErrorCode::InvalidState, "Global Gate and Storage admission state disagree"));
            deferred_result.emplace(
                pre_rejected(*this, RODAQ_REJECTION_RUNTIME_FAILURE, disposition));
        } else {
            ++admission_inflight_;
            final_binding_entry->admission_inflight->fetch_add(1);
            auto sequence = sequences_.Allocate(stream_handle);
            if (!sequence.has_value()) {
                --admission_inflight_;
                final_binding_entry->admission_inflight->fetch_sub(1);
                admission_condition_.notify_all();
                deferred_failure.emplace(sequence.error());
                deferred_result.emplace(submit_result(
                    RODAQ_ADMISSION_REJECTED, RODAQ_REJECTION_RUNTIME_FAILURE, disposition));
            } else {
                auto record = RuntimeRecord::Create(
                    final_binding, sequence.value(), source, source_sequence, timestamps,
                    std::move(payload), reservation.TakeRecordReservation());
                if (!record.has_value()) {
                    --admission_inflight_;
                    final_binding_entry->admission_inflight->fetch_sub(1);
                    admission_condition_.notify_all();
                    stats_.RecordRejected();
                    deferred_failure.emplace(record.error());
                    deferred_result.emplace(submit_result(
                        RODAQ_ADMISSION_REJECTED, RODAQ_REJECTION_RUNTIME_FAILURE, disposition));
                } else {
                    stats_.RecordInflightAcquired(static_cast<std::uint64_t>(payload_size));
                    pipeline_->Publish(std::move(reservation), std::move(record.value()));
                    stats_.RecordAccepted(payload_size);
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
    if (deferred_failure.has_value()) {
        RequestFailure(std::move(deferred_failure.value()));
    }
    if (retire_binding) {
        RetireDrainingBinding(current_binding);
    }
    if (deferred_result.has_value()) {
        return deferred_result.value();
    }
    return submit_result(RODAQ_ADMISSION_ACCEPTED, RODAQ_REJECTION_NONE, disposition);
}

}  // namespace rodaq::runtime
