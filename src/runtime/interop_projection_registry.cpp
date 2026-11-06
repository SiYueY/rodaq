#include "runtime/interop_projection_registry.hpp"

#include <rodaq/plugin/abi_support.hpp>

#include <rodaq/status.hpp>

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

Status projection_status(StatusCode status_code, std::string message) {
    return Status(status_code, "projection", 0, std::move(message));
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

void InteropProjectionRegistry::set_registration_sink(RegistrationSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    registration_sink_ = std::move(sink);
}

Result<void> InteropProjectionRegistry::register_entry(
    rodaq_stream_descriptor_handle descriptor, std::string_view projection_id,
    const std::uint8_t* payload, std::size_t payload_size) {
    if (!limits_are_positive(limits_)) {
        return projection_status(
            StatusCode::InvalidState, "Projection Registry limits must be positive");
    }
    if (descriptor.value == 0) {
        return projection_status(
            StatusCode::InvalidArgument, "Projection descriptor handle is invalid");
    }
    if (projection_id.empty()) {
        return projection_status(StatusCode::InvalidArgument, "Projection ID must not be empty");
    }
    if (projection_id.size() > limits_.max_projection_id_bytes) {
        return projection_status(
            StatusCode::ResourceExhausted, "Projection ID exceeds the configured byte limit");
    }
    if (!rodaq::is_valid_utf8_without_nul(projection_id)) {
        return projection_status(
            StatusCode::InvalidArgument, "Projection ID is not valid UTF-8 without NUL");
    }
    if (payload_size != 0 && payload == nullptr) {
        return projection_status(StatusCode::InvalidArgument, "Projection payload has null data");
    }

    RegistrationSink sink;
    {
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
            return projection_status(
                StatusCode::Conflict, "Projection already exists with different bytes");
        }

        std::size_t descriptor_count = 0;
        std::size_t descriptor_bytes = 0;
        for (const std::unique_ptr<Entry>& entry : entries_) {
            if (entry->descriptor.value != descriptor.value) {
                continue;
            }
            ++descriptor_count;
            if (entry->payload.size() >
                std::numeric_limits<std::size_t>::max() - descriptor_bytes) {
                return projection_status(
                    StatusCode::InternalError, "Projection byte accounting overflowed");
            }
            descriptor_bytes += entry->payload.size();
        }
        if (descriptor_count >= limits_.max_projection_count_per_descriptor) {
            return projection_status(
                StatusCode::ResourceExhausted, "Projection count limit is exhausted");
        }
        if (payload_size > std::numeric_limits<std::size_t>::max() - descriptor_bytes ||
            descriptor_bytes + payload_size > limits_.max_projection_bytes_per_descriptor) {
            return projection_status(
                StatusCode::ResourceExhausted, "Projection descriptor byte limit is exhausted");
        }
        if (payload_size > std::numeric_limits<std::size_t>::max() - total_payload_bytes_ ||
            total_payload_bytes_ + payload_size > limits_.max_projection_bytes_per_activation) {
            return projection_status(
                StatusCode::ResourceExhausted, "Projection activation byte limit is exhausted");
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
            return projection_status(
                StatusCode::ResourceExhausted, "Memory exhausted while storing a Projection");
        }
        sink = registration_sink_;
    }

    if (sink) {
        try {
            sink(descriptor, projection_id);
        } catch (...) {
        }
    }
    return {};
}

Result<ProjectionView> InteropProjectionRegistry::get(
    rodaq_stream_descriptor_handle descriptor, std::string_view projection_id) const {
    if (descriptor.value == 0 || projection_id.empty() ||
        !rodaq::is_valid_utf8_without_nul(projection_id) ||
        projection_id.size() > limits_.max_projection_id_bytes) {
        return projection_status(StatusCode::InvalidArgument, "Projection lookup key is invalid");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(
        entries_.begin(), entries_.end(),
        [descriptor, projection_id](const std::unique_ptr<Entry>& entry) {
            return entry->descriptor.value == descriptor.value &&
                   entry->projection_id == projection_id;
        });
    if (found == entries_.end()) {
        return projection_status(StatusCode::NotFound, "Projection was not found");
    }
    return ProjectionView{
        (*found)->projection_id,
        (*found)->payload.empty() ? nullptr : (*found)->payload.data(),
        (*found)->payload.size(),
    };
}

rodaq_interface InteropProjectionRegistry::abi_interface() noexcept {
    return rodaq_interface{this, &kProjectionVtable};
}

rodaq_status_code InteropProjectionRegistry::register_projection(
    void* context, const rodaq_projection_registration* request,
    rodaq_status* out_status) noexcept {
    if (context == nullptr || request == nullptr ||
        request->struct_size < RODAQ_PROJECTION_REGISTRATION_MIN_STRUCT_SIZE) {
        assign_abi_status(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0,
            "Projection registration has an invalid context or request", out_status);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    if (request->reserved != 0) {
        assign_abi_status(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0,
            "Projection registration reserved field is non-zero", out_status);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    std::string_view projection_id;
    if (!parse_abi_string(request->projection_id, &projection_id)) {
        assign_abi_status(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0, "Projection ID is not valid UTF-8",
            out_status);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (!parse_abi_bytes(request->payload, &payload, &payload_size)) {
        assign_abi_status(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0, "Projection payload is invalid",
            out_status);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    try {
        auto* registry = static_cast<InteropProjectionRegistry*>(context);
        Result<void> result =
            registry->register_entry(request->descriptor, projection_id, payload, payload_size);
        if (!result.ok()) {
            assign_abi_status(result.status(), out_status);
            return abi_status_from_status_code(result.status().status_code());
        }
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        assign_abi_status(RODAQ_STATUS_INTERNAL_ERROR, "projection", 0, error.what(), out_status);
        return RODAQ_STATUS_INTERNAL_ERROR;
    } catch (...) {
        assign_abi_status(
            RODAQ_STATUS_INTERNAL_ERROR, "projection", 0, "Unknown Projection registration failure",
            out_status);
        return RODAQ_STATUS_INTERNAL_ERROR;
    }
}

rodaq_status_code InteropProjectionRegistry::get_projection(
    void* context, rodaq_stream_descriptor_handle descriptor, rodaq_string_view projection_id,
    rodaq_projection_view* out_view, rodaq_status* out_status) noexcept {
    if (out_view == nullptr) {
        assign_abi_status(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0,
            "Projection query requires a non-null output view", out_status);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const std::uint32_t capacity = out_view->struct_size;
    if (capacity < RODAQ_PROJECTION_VIEW_MIN_STRUCT_SIZE) {
        out_view->struct_size = 0;
        assign_abi_status(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0,
            "Projection output capacity is below the V1 minimum", out_status);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const std::uint32_t initialized =
        rodaq::initialized_prefix_size<rodaq_projection_view>(capacity);
    std::memset(out_view, 0, initialized);
    if (context == nullptr) {
        assign_abi_status(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0, "Projection query context is null",
            out_status);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    std::string_view parsed_id;
    if (!parse_abi_string(projection_id, &parsed_id)) {
        assign_abi_status(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection", 0, "Projection ID is not valid UTF-8",
            out_status);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    try {
        const auto* registry = static_cast<const InteropProjectionRegistry*>(context);
        Result<ProjectionView> result = registry->get(descriptor, parsed_id);
        if (!result.ok()) {
            assign_abi_status(result.status(), out_status);
            return abi_status_from_status_code(result.status().status_code());
        }
        const ProjectionView& view = result.value();
        out_view->struct_size = initialized;
        out_view->reserved = 0;
        if (RODAQ_HAS_FIELD_CAPACITY(rodaq_projection_view, capacity, descriptor)) {
            out_view->descriptor = descriptor;
        }
        if (RODAQ_HAS_FIELD_CAPACITY(rodaq_projection_view, capacity, projection_id)) {
            out_view->projection_id =
                rodaq_string_view{view.projection_id.data(), view.projection_id.size()};
        }
        if (RODAQ_HAS_FIELD_CAPACITY(rodaq_projection_view, capacity, payload)) {
            out_view->payload = rodaq_bytes_view{view.payload, view.payload_size};
        }
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        assign_abi_status(RODAQ_STATUS_INTERNAL_ERROR, "projection", 0, error.what(), out_status);
        return RODAQ_STATUS_INTERNAL_ERROR;
    } catch (...) {
        assign_abi_status(
            RODAQ_STATUS_INTERNAL_ERROR, "projection", 0, "Unknown Projection query failure",
            out_status);
        return RODAQ_STATUS_INTERNAL_ERROR;
    }
}

}  // namespace rodaq::runtime
