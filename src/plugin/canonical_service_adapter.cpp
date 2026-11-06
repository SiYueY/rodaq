#include "activation/activation_state.hpp"
#include "plugin/abi_conversion.hpp"

#include <exception>
#include <string_view>
#include <utility>

namespace rodaq {
namespace {

rodaq_status_code assign_status(const Status& error, rodaq_status* out_status) noexcept {
    assign_abi_status(error, out_status);
    return abi_status_from_status_code(error.status_code());
}

rodaq_status_code assign_message(
    StatusCode status_code, std::string_view message, rodaq_status* out_status) noexcept {
    assign_abi_status(abi_status_from_status_code(status_code), "plugin", 0, message, out_status);
    return abi_status_from_status_code(status_code);
}

ActivationState* state_from(void* context) noexcept {
    return static_cast<ActivationState*>(context);
}

template <typename Info, typename Getter>
rodaq_status_code canonical_view(
    void* context, Info* out_info, rodaq_status* out_status, Getter&& getter) noexcept {
    if (context == nullptr || out_info == nullptr) {
        return assign_message(
            StatusCode::InvalidArgument, "canonical info input is null", out_status);
    }
    try {
        const auto result = getter(*state_from(context));
        if (!result.ok()) {
            return assign_status(result.status(), out_status);
        }
        const auto copied = write_output_prefix(result.value(), out_info);
        return copied.ok() ? RODAQ_STATUS_OK : assign_status(copied.status(), out_status);
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(StatusCode::InternalError, "canonical info query failed", out_status);
    }
}

rodaq_status_code canonical_get_source(
    void* context, rodaq_source_handle handle, rodaq_source_info* out_info,
    rodaq_status* out_status) noexcept {
    return canonical_view(context, out_info, out_status, [handle](ActivationState& state) {
        rodaq_source_info info{};
        info.struct_size = sizeof(info);
        auto result = state.get_source(SourceHandle::from_raw(handle.value), info);
        if (!result.ok()) {
            return Result<rodaq_source_info>(std::move(result).status());
        }
        return Result<rodaq_source_info>(info);
    });
}

rodaq_status_code canonical_get_clock(
    void* context, rodaq_clock_handle handle, rodaq_clock_info* out_info,
    rodaq_status* out_status) noexcept {
    return canonical_view(context, out_info, out_status, [handle](ActivationState& state) {
        rodaq_clock_info info{};
        info.struct_size = sizeof(info);
        auto result = state.get_clock(ClockHandle::from_raw(handle.value), info);
        if (!result.ok()) {
            return Result<rodaq_clock_info>(std::move(result).status());
        }
        return Result<rodaq_clock_info>(info);
    });
}

rodaq_status_code canonical_get_schema(
    void* context, rodaq_schema_handle handle, rodaq_schema_info* out_info,
    rodaq_status* out_status) noexcept {
    return canonical_view(context, out_info, out_status, [handle](ActivationState& state) {
        rodaq_schema_info info{};
        info.struct_size = sizeof(info);
        auto result = state.get_schema(SchemaHandle::from_raw(handle.value), info);
        if (!result.ok()) {
            return Result<rodaq_schema_info>(std::move(result).status());
        }
        return Result<rodaq_schema_info>(info);
    });
}

rodaq_status_code canonical_get_timestamp_key_set(
    void* context, std::uint64_t id, rodaq_timestamp_key_set_info* out_info,
    rodaq_status* out_status) noexcept {
    return canonical_view(context, out_info, out_status, [id](ActivationState& state) {
        rodaq_timestamp_key_set_info info{};
        info.struct_size = sizeof(info);
        auto result = state.get_timestamp_key_set(id, info);
        if (!result.ok()) {
            return Result<rodaq_timestamp_key_set_info>(std::move(result).status());
        }
        return Result<rodaq_timestamp_key_set_info>(info);
    });
}

rodaq_status_code canonical_get_stream(
    void* context, rodaq_stream_handle handle, rodaq_stream_info* out_info,
    rodaq_status* out_status) noexcept {
    return canonical_view(context, out_info, out_status, [handle](ActivationState& state) {
        rodaq_stream_info info{};
        info.struct_size = sizeof(info);
        auto result = state.get_stream(StreamHandle::from_raw(handle.value), info);
        if (!result.ok()) {
            return Result<rodaq_stream_info>(std::move(result).status());
        }
        return Result<rodaq_stream_info>(info);
    });
}

rodaq_status_code canonical_get_stream_descriptor(
    void* context, rodaq_stream_descriptor_handle handle, rodaq_stream_descriptor_info* out_info,
    rodaq_status* out_status) noexcept {
    return canonical_view(context, out_info, out_status, [handle](ActivationState& state) {
        rodaq_stream_descriptor_info info{};
        info.struct_size = sizeof(info);
        auto result =
            state.get_stream_descriptor(StreamDescriptorHandle::from_raw(handle.value), info);
        if (!result.ok()) {
            return Result<rodaq_stream_descriptor_info>(std::move(result).status());
        }
        return Result<rodaq_stream_descriptor_info>(info);
    });
}

}  // namespace

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

}  // namespace rodaq
