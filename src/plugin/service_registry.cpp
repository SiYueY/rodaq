#include "plugin/service_registry.hpp"

#include <rodaq/plugin/abi_support.hpp>

#include <rodaq/status.hpp>

#include <algorithm>
#include <exception>
#include <new>
#include <string>
#include <utility>

namespace rodaq::plugin {
const rodaq_service_registry_vtable ServiceRegistry::kRegistryVtable = {
    RODAQ_SERVICE_REGISTRY_PROTOCOL_VERSION,
    RODAQ_SERVICE_REGISTRY_VTABLE_MIN_STRUCT_SIZE,
    &ServiceRegistry::query,
};

ServiceRegistry::ServiceRegistry(std::size_t max_service_count, std::size_t max_service_id_bytes)
: max_service_count_(max_service_count), max_service_id_bytes_(max_service_id_bytes) {
    services_.reserve(max_service_count);
}

Result<void> ServiceRegistry::register_entry(std::string_view service_id, rodaq_interface service) {
    if (service_id.empty()) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "service_registry", 0, "Service ID must not be empty");
    }
    if (service_id.size() > max_service_id_bytes_) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "service_registry", 0,
            "Service ID exceeds the configured length limit");
    }
    if (!rodaq::is_valid_utf8_without_nul(service_id)) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "service_registry", 0,
            "Service ID is not valid UTF-8 without NUL");
    }
    if (service.context == nullptr || service.vtable == nullptr) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "service_registry", 0,
            "Service interface must have non-null context and vtable");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (frozen_) {
        return rodaq::Status(
            StatusCode::InvalidState, "service_registry", 0, "Service Registry is frozen");
    }
    const bool duplicate = std::any_of(
        services_.begin(), services_.end(),
        [service_id](const Entry& entry) { return entry.service_id == service_id; });
    if (duplicate) {
        return rodaq::Status(
            StatusCode::Conflict, "service_registry", 0, "Service ID is already registered");
    }
    if (services_.size() >= max_service_count_) {
        return rodaq::Status(
            StatusCode::ResourceExhausted, "service_registry", 0,
            "Service Registry capacity is exhausted");
    }

    try {
        services_.push_back(Entry{std::string(service_id), service});
    } catch (const std::bad_alloc&) {
        return rodaq::Status(
            StatusCode::ResourceExhausted, "service_registry", 0,
            "Memory exhausted while registering a Service");
    }
    return {};
}

Result<void> ServiceRegistry::freeze() {
    std::lock_guard<std::mutex> lock(mutex_);
    frozen_ = true;
    return {};
}

Result<rodaq_interface> ServiceRegistry::query(std::string_view service_id) const {
    if (service_id.empty() || !rodaq::is_valid_utf8_without_nul(service_id) ||
        service_id.size() > max_service_id_bytes_) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "service_registry", 0, "Service ID is invalid");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!frozen_) {
        return rodaq::Status(
            StatusCode::InvalidState, "service_registry", 0,
            "Service Registry must be frozen before query");
    }
    const auto found = std::find_if(
        services_.begin(), services_.end(),
        [service_id](const Entry& entry) { return entry.service_id == service_id; });
    if (found == services_.end()) {
        return rodaq::Status(
            StatusCode::NotFound, "service_registry", 0, "Service ID is not registered");
    }
    return found->service;
}

rodaq_service_registry ServiceRegistry::abi_view() noexcept {
    return rodaq_service_registry{this, &kRegistryVtable};
}

rodaq_status_code ServiceRegistry::query(
    void* context, rodaq_string_view service_id, rodaq_interface* out_service,
    rodaq_status* out_status) noexcept {
    if (out_service == nullptr) {
        assign_abi_status(
            RODAQ_STATUS_INVALID_ARGUMENT, "service_registry", 0,
            "Service Registry query requires a non-null output interface", out_status);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_service = {};

    if (context == nullptr) {
        assign_abi_status(
            RODAQ_STATUS_INVALID_ARGUMENT, "service_registry", 0,
            "Service Registry query context is null", out_status);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    std::string_view parsed_id;
    if (!parse_abi_string(service_id, &parsed_id)) {
        assign_abi_status(
            RODAQ_STATUS_INVALID_ARGUMENT, "service_registry", 0, "Service ID is not valid UTF-8",
            out_status);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    try {
        const auto* registry = static_cast<const ServiceRegistry*>(context);
        Result<rodaq_interface> result = registry->query(parsed_id);
        if (!result.ok()) {
            assign_abi_status(result.status(), out_status);
            return abi_status_from_status_code(result.status().status_code());
        }
        *out_service = result.value();
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        assign_abi_status(
            RODAQ_STATUS_INTERNAL_ERROR, "service_registry", 0, error.what(), out_status);
        return RODAQ_STATUS_INTERNAL_ERROR;
    } catch (...) {
        assign_abi_status(
            RODAQ_STATUS_INTERNAL_ERROR, "service_registry", 0,
            "Unknown Service Registry query failure", out_status);
        return RODAQ_STATUS_INTERNAL_ERROR;
    }
}

}  // namespace rodaq::plugin
