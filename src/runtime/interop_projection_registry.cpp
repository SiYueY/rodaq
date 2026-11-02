#include "runtime/interop_projection_registry.hpp"

#include <rodaq/plugin/abi_support.hpp>

#include <rodaq/error.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace rodaq::runtime {
namespace {

bool limits_are_positive(const ProjectionLimits& limits) noexcept {
    return limits.max_projection_id_bytes > 0 && limits.max_projection_count_per_descriptor > 0 &&
           limits.max_projection_bytes_per_descriptor > 0 &&
           limits.max_projection_bytes_per_activation > 0;
}

Error projection_error(ErrorCode code, std::string message) {
    return Error(code, "projection", 0, std::move(message));
}

bool same_payload(
    const std::vector<std::uint8_t>& stored, const std::uint8_t* payload,
    std::size_t payload_size) noexcept {
    return stored.size() == payload_size &&
           (payload_size == 0 || std::memcmp(stored.data(), payload, payload_size) == 0);
}

}  // namespace

const rodaq_runtime_projection_v1 InteropProjectionRegistry::kProjectionVtable = {
    RODAQ_RUNTIME_PROJECTION_PROTOCOL_VERSION,
    RODAQ_RUNTIME_PROJECTION_V1_MIN_STRUCT_SIZE,
    &InteropProjectionRegistry::register_projection,
    &InteropProjectionRegistry::get_projection,
};

InteropProjectionRegistry::InteropProjectionRegistry(ProjectionLimits limits) : limits_(limits) {}

Result<void> InteropProjectionRegistry::Register(
    rodaq_stream_descriptor_handle descriptor, std::string_view projection_id,
    const std::uint8_t* payload, std::size_t payload_size) {
    if (!limits_are_positive(limits_)) {
        return nonstd::make_unexpected(projection_error(
            ErrorCode::InvalidState, "Projection Registry limits must be positive"));
    }
    if (descriptor.value == 0) {
        return nonstd::make_unexpected(projection_error(
            ErrorCode::InvalidArgument, "Projection descriptor handle is invalid"));
    }
    if (projection_id.empty()) {
        return nonstd::make_unexpected(
            projection_error(ErrorCode::InvalidArgument, "Projection ID must not be empty"));
    }
    if (projection_id.size() > limits_.max_projection_id_bytes) {
        return nonstd::make_unexpected(projection_error(
            ErrorCode::ResourceExhausted, "Projection ID exceeds the configured byte limit"));
    }
    if (!rodaq::is_valid_utf8_without_nul(projection_id)) {
        return nonstd::make_unexpected(projection_error(
            ErrorCode::InvalidArgument, "Projection ID is not valid UTF-8 without NUL"));
    }
    if (payload_size != 0 && payload == nullptr) {
        return nonstd::make_unexpected(
            projection_error(ErrorCode::InvalidArgument, "Projection payload has null data"));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = std::find_if(
        entries_.begin(), entries_.end(),
        [descriptor, projection_id](const std::unique_ptr<Entry>& entry) {
            return entry->descriptor.value == descriptor.value &&
                   entry->projection_id == projection_id;
        });
    if (existing != entries_.end()) {
        if (same_payload((*existing)->payload, payload, payload_size)) {
            return {};
        }
        return nonstd::make_unexpected(projection_error(
            ErrorCode::Conflict, "Projection already exists with different bytes"));
    }

    std::size_t descriptor_count = 0;
    std::size_t descriptor_bytes = 0;
    for (const std::unique_ptr<Entry>& entry : entries_) {
        if (entry->descriptor.value != descriptor.value) {
            continue;
        }
        ++descriptor_count;
        if (entry->payload.size() > std::numeric_limits<std::size_t>::max() - descriptor_bytes) {
            return nonstd::make_unexpected(projection_error(
                ErrorCode::InternalError, "Projection byte accounting overflowed"));
        }
        descriptor_bytes += entry->payload.size();
    }
    if (descriptor_count >= limits_.max_projection_count_per_descriptor) {
        return nonstd::make_unexpected(
            projection_error(ErrorCode::ResourceExhausted, "Projection count limit is exhausted"));
    }
    if (payload_size > std::numeric_limits<std::size_t>::max() - descriptor_bytes ||
        descriptor_bytes + payload_size > limits_.max_projection_bytes_per_descriptor) {
        return nonstd::make_unexpected(projection_error(
            ErrorCode::ResourceExhausted, "Projection descriptor byte limit is exhausted"));
    }
    if (payload_size > std::numeric_limits<std::size_t>::max() - total_payload_bytes_ ||
        total_payload_bytes_ + payload_size > limits_.max_projection_bytes_per_activation) {
        return nonstd::make_unexpected(projection_error(
            ErrorCode::ResourceExhausted, "Projection activation byte limit is exhausted"));
    }

    try {
        auto entry = std::make_unique<Entry>();
        entry->descriptor = descriptor;
        entry->projection_id = std::string(projection_id);
        if (payload_size != 0) {
            entry->payload.assign(payload, payload + payload_size);
        }
        entries_.push_back(std::move(entry));
        total_payload_bytes_ += payload_size;
    } catch (const std::bad_alloc&) {
        return nonstd::make_unexpected(projection_error(
            ErrorCode::ResourceExhausted, "Memory exhausted while storing a Projection"));
    }
    return {};
}

Result<ProjectionView> InteropProjectionRegistry::Get(
    rodaq_stream_descriptor_handle descriptor, std::string_view projection_id) const {
    if (descriptor.value == 0 || projection_id.empty() ||
        !rodaq::is_valid_utf8_without_nul(projection_id) ||
        projection_id.size() > limits_.max_projection_id_bytes) {
        return nonstd::make_unexpected(
            projection_error(ErrorCode::InvalidArgument, "Projection lookup key is invalid"));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(
        entries_.begin(), entries_.end(),
        [descriptor, projection_id](const std::unique_ptr<Entry>& entry) {
            return entry->descriptor.value == descriptor.value &&
                   entry->projection_id == projection_id;
        });
    if (found == entries_.end()) {
        return nonstd::make_unexpected(
            projection_error(ErrorCode::NotFound, "Projection was not found"));
    }
    return ProjectionView{
        (*found)->projection_id,
        (*found)->payload.empty() ? nullptr : (*found)->payload.data(),
        (*found)->payload.size(),
    };
}

rodaq_interface InteropProjectionRegistry::AbiInterface() noexcept {
    return rodaq_interface{this, &kProjectionVtable};
}

rodaq_status InteropProjectionRegistry::register_projection(
    void* context, const rodaq_projection_registration* request, rodaq_error* out_error) noexcept {
    if (context == nullptr || request == nullptr ||
        request->struct_size < RODAQ_PROJECTION_REGISTRATION_MIN_STRUCT_SIZE) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0,
            "Projection registration has an invalid context or request", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    if (request->reserved != 0) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0,
            "Projection registration reserved field is non-zero", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    std::string_view projection_id;
    if (!parse_abi_string(request->projection_id, &projection_id)) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0, "Projection ID is not valid UTF-8",
            out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (!parse_abi_bytes(request->payload, &payload, &payload_size)) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0, "Projection payload is invalid",
            out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    try {
        auto* registry = static_cast<InteropProjectionRegistry*>(context);
        Result<void> result =
            registry->Register(request->descriptor, projection_id, payload, payload_size);
        if (!result.has_value()) {
            assign_rodaq_error(result.error(), out_error);
            return status_from_error_code(result.error().code());
        }
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        assign_rodaq_error(RODAQ_STATUS_INTERNAL_ERROR, "projection", 0, error.what(), out_error);
        return RODAQ_STATUS_INTERNAL_ERROR;
    } catch (...) {
        assign_rodaq_error(
            RODAQ_STATUS_INTERNAL_ERROR, "projection", 0, "Unknown Projection registration failure",
            out_error);
        return RODAQ_STATUS_INTERNAL_ERROR;
    }
}

rodaq_status InteropProjectionRegistry::get_projection(
    void* context, rodaq_stream_descriptor_handle descriptor, rodaq_string_view projection_id,
    rodaq_projection_view* out_view, rodaq_error* out_error) noexcept {
    if (out_view == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0,
            "Projection query requires a non-null output view", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const std::uint32_t capacity = out_view->struct_size;
    if (capacity < RODAQ_PROJECTION_VIEW_MIN_STRUCT_SIZE) {
        out_view->struct_size = 0;
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0,
            "Projection output capacity is below the V1 minimum", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_view = {};
    if (context == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0, "Projection query context is null",
            out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    std::string_view parsed_id;
    if (!parse_abi_string(projection_id, &parsed_id)) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0, "Projection ID is not valid UTF-8",
            out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    try {
        const auto* registry = static_cast<const InteropProjectionRegistry*>(context);
        Result<ProjectionView> result = registry->Get(descriptor, parsed_id);
        if (!result.has_value()) {
            assign_rodaq_error(result.error(), out_error);
            return status_from_error_code(result.error().code());
        }
        const ProjectionView& view = result.value();
        out_view->struct_size = sizeof(rodaq_projection_view);
        out_view->reserved = 0;
        out_view->descriptor = descriptor;
        out_view->projection_id =
            rodaq_string_view{view.projection_id.data(), view.projection_id.size()};
        out_view->payload = rodaq_bytes_view{view.payload, view.payload_size};
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        assign_rodaq_error(RODAQ_STATUS_INTERNAL_ERROR, "projection", 0, error.what(), out_error);
        return RODAQ_STATUS_INTERNAL_ERROR;
    } catch (...) {
        assign_rodaq_error(
            RODAQ_STATUS_INTERNAL_ERROR, "projection", 0, "Unknown Projection query failure",
            out_error);
        return RODAQ_STATUS_INTERNAL_ERROR;
    }
}

}  // namespace rodaq::runtime
