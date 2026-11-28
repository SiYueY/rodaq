#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rodaq/plugin/abi.hpp>
#include <rodaq/result.hpp>

namespace rodaq::plugin::authoring {

// SDK-local bounded binding table; providers remain owned by their plugin instance.
inline constexpr std::size_t kMaxInterfaces = 64;

struct InterfaceEntry {
    std::string id;
    rodaq_interface_reference_t reference{};
};

inline rodaq_status_code_t encode_status(const Status& status, rodaq_error_t* error) noexcept {
    const auto code = static_cast<rodaq_status_code_t>(status.code());
    if (!abi::known_status(code)) {
        abi::write_error(error, "Unknown SDK status");
        return RODAQ_STATUS_INTERNAL;
    }
    abi::write_error(error, status.ok() ? std::string_view{} : status.message());
    return code;
}

template <typename Function>
rodaq_status_code_t exception_boundary(rodaq_error_t* error, Function&& function) noexcept {
    if (!abi::valid_error_input(error)) {
        error->message.size = 0;
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    abi::write_error(error, {});
    try {
        const auto result = function();
        return encode_status(result.status(), error);
    } catch (const std::bad_alloc&) {
        abi::write_error(error, "Plugin allocation failed");
        return RODAQ_STATUS_RESOURCE_EXHAUSTED;
    } catch (...) {
        abi::write_error(error, "Plugin exception");
        return RODAQ_STATUS_INTERNAL;
    }
}

template <typename Interface, typename Implementation>
Result<InterfaceEntry> bind_interface(Implementation& implementation) {
    if (!abi::valid_identifier(Interface::kInterfaceId)) {
        return Status(StatusCode::InvalidArgument, "Invalid Interface ID");
    }
    using Provider = typename Interface::template Provider<Implementation>;
    const auto* table = Provider::vtable();
    rodaq_interface_reference_t reference{
        reinterpret_cast<rodaq_interface_context_t*>(&implementation),
        reinterpret_cast<const rodaq_interface_vtable_t*>(table),
        sizeof(*table),
        {Interface::kMajor, Interface::kMinor}};
    typename Interface::Vtable checked{};
    if (!abi::copy_vtable<Interface>(reference, checked)) {
        return Status(StatusCode::InvalidArgument, "Invalid Provider vtable");
    }
    return InterfaceEntry{std::string(Interface::kInterfaceId), reference};
}

template <typename Interface, typename Implementation>
Result<void> add_interface(std::vector<InterfaceEntry>& entries, Implementation& implementation) {
    for (const auto& entry : entries) {
        if (entry.id == Interface::kInterfaceId &&
            entry.reference.negotiated_version.major == Interface::kMajor) {
            return Status(StatusCode::Conflict, "Duplicate Interface ID/major");
        }
    }
    if (entries.size() >= kMaxInterfaces) {
        return Status(StatusCode::ResourceExhausted, "Interface table full");
    }
    auto binding = bind_interface<Interface>(implementation);
    if (!binding.ok()) {
        return binding.status();
    }
    entries.push_back(std::move(binding).value());
    return {};
}

inline rodaq_status_code_t query_interface(
    const std::vector<InterfaceEntry>& entries, const rodaq_interface_query_t* query,
    rodaq_interface_reference_t* reference, rodaq_error_t* error) noexcept {
    abi::write_error(error, {});
    if (reference == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *reference = {};
    if (!abi::valid_error_input(error) || query == nullptr || !abi::valid_query(*query)) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const std::string_view id(query->interface_id.data, query->interface_id.size);
    bool found = false;
    for (const auto& entry : entries) {
        if (entry.id != id) {
            continue;
        }
        found = true;
        const auto version = entry.reference.negotiated_version;
        if (version.major != query->major || version.minor < query->min_minor) {
            continue;
        }
        *reference = entry.reference;
        reference->negotiated_version.minor = std::min(version.minor, query->max_minor);
        return RODAQ_STATUS_OK;
    }
    return found ? RODAQ_STATUS_UNAVAILABLE : RODAQ_STATUS_NOT_FOUND;
}

inline bool valid_application_callbacks(const rodaq_application_callbacks_t* callbacks) noexcept {
    return abi::readable(
        callbacks, offsetof(rodaq_application_callbacks_t, log) + sizeof(callbacks->log));
}

}  // namespace rodaq::plugin::authoring
