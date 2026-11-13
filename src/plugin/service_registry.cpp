#include <rodaq/plugin/service_registry.hpp>

#include "abi_support.hpp"
#include "service_registry_state.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace rodaq::plugin {
namespace {

rodaq_status_code_t registry_query(
    rodaq_service_registry_context_t* context, const rodaq_interface_query_t* query,
    rodaq_interface_reference_t* interface, rodaq_error_t* error) noexcept {
    if (interface != nullptr) {
        *interface = rodaq_interface_reference_t{};
    }
    if (interface == nullptr || query == nullptr || !valid_error_buffer(error)) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    clear_error(error);

    if (context == nullptr || !valid_string(query->interface_id) ||
        query->min_minor > query->max_minor) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    const auto* state = reinterpret_cast<const ServiceRegistryState*>(context);
    auto result = state->query(
        string_view_from_abi(query->interface_id), query->major, query->min_minor,
        query->max_minor);
    if (!result.ok()) {
        return result.status().code() == StatusCode::NotFound      ? RODAQ_STATUS_NOT_FOUND
               : result.status().code() == StatusCode::Unavailable ? RODAQ_STATUS_UNAVAILABLE
                                                                   : RODAQ_STATUS_INVALID_STATE;
    }

    *interface = result.value();
    return RODAQ_STATUS_OK;
}

rodaq_status_code_t registry_enter_call(
    rodaq_service_registry_context_t*, rodaq_error_t* error) noexcept {
    if (!valid_error_buffer(error)) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    clear_error(error);
    enter_managed_call();
    return RODAQ_STATUS_OK;
}

void registry_leave_call(rodaq_service_registry_context_t*) noexcept { leave_managed_call(); }

const rodaq_service_registry_vtable_t kRegistryVtable{
    rodaq_struct_header_t{sizeof(rodaq_service_registry_vtable_t)}, &registry_query,
    &registry_enter_call, &registry_leave_call};

}  // namespace

ServiceRegistryState::~ServiceRegistryState() = default;

Result<void> ServiceRegistryState::add(Entry entry) {
    std::lock_guard<std::mutex> lock{mutex_};
    if (frozen_) {
        return Status{StatusCode::InvalidState, "service registry is frozen"};
    }
    if (entry.interface_id.empty() || entry.context == nullptr || entry.vtable == nullptr ||
        entry.vtable_size < sizeof(rodaq_struct_header_t) || !valid_utf8_text(entry.interface_id)) {
        return Status{StatusCode::InvalidArgument, "service entry is incomplete"};
    }
    for (const auto& existing : entries_) {
        if (existing.interface_id == entry.interface_id && existing.major == entry.major) {
            return Status{StatusCode::Conflict, "service interface already registered"};
        }
    }
    entries_.push_back(std::move(entry));
    return {};
}

Result<void> ServiceRegistryState::freeze() {
    std::lock_guard<std::mutex> lock{mutex_};
    frozen_ = true;
    return {};
}

bool ServiceRegistryState::frozen() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return frozen_;
}

Result<rodaq_interface_reference_t> ServiceRegistryState::query(
    std::string_view interface_id, std::uint16_t major, std::uint16_t min_minor,
    std::uint16_t max_minor) const {
    std::lock_guard<std::mutex> lock{mutex_};

    const Entry* best = nullptr;
    bool interface_exists = false;
    bool major_exists = false;
    for (const auto& entry : entries_) {
        if (entry.interface_id != interface_id) {
            continue;
        }
        interface_exists = true;
        if (entry.major != major) {
            continue;
        }
        major_exists = true;
        if (entry.minor < min_minor || min_minor > max_minor) {
            continue;
        }
        if (best == nullptr || entry.minor > best->minor) {
            best = &entry;
        }
    }

    if (best == nullptr) {
        if (!interface_exists) {
            return Status{StatusCode::NotFound, "service interface was not found"};
        }
        if (!major_exists || min_minor > max_minor) {
            return Status{StatusCode::Unavailable, "service version is incompatible"};
        }
        return Status{StatusCode::Unavailable, "service version is incompatible"};
    }

    const auto negotiated_minor = std::min(best->minor, max_minor);
    return rodaq_interface_reference_t{
        reinterpret_cast<rodaq_interface_context_t*>(best->context),
        reinterpret_cast<const rodaq_interface_vtable_t*>(best->vtable), best->vtable_size,
        rodaq_interface_version_t{major, negotiated_minor}};
}

const rodaq_service_registry_vtable_t* ServiceRegistryState::vtable() noexcept {
    return &kRegistryVtable;
}

rodaq_service_registry_reference_t make_service_registry_reference(
    const std::shared_ptr<const ServiceRegistryState>& state) noexcept {
    return rodaq_service_registry_reference_t{
        // Plugin receives an opaque non-const pointer because the C ABI exposes
        // a conventional context type, but the immutable Registry never mutates.
        const_cast<rodaq_service_registry_context_t*>(
            reinterpret_cast<const rodaq_service_registry_context_t*>(state.get())),
        ServiceRegistryState::vtable()};
}

ServiceRegistry::ServiceRegistry() : state_(std::make_shared<ServiceRegistryState>()) {}

ServiceRegistry::ServiceRegistry(ServiceRegistry&&) noexcept = default;
ServiceRegistry& ServiceRegistry::operator=(ServiceRegistry&&) noexcept = default;
ServiceRegistry::~ServiceRegistry() = default;

Result<void> ServiceRegistry::add_entry(
    std::string_view interface_id, std::uint16_t major, std::uint16_t minor, void* context,
    const void* vtable, std::uint32_t vtable_size, std::shared_ptr<const void> owner) {
    if (state_ == nullptr || interface_id.empty()) {
        return Status{StatusCode::InvalidState, "service registry is empty"};
    }
    return state_->add(ServiceRegistryState::Entry{
        std::string{interface_id}, major, minor, context, vtable, vtable_size, std::move(owner)});
}

Result<void> ServiceRegistry::freeze() {
    if (state_ == nullptr) {
        return Status{StatusCode::InvalidState, "service registry is empty"};
    }
    return state_->freeze();
}

bool ServiceRegistry::frozen() const noexcept { return state_ != nullptr && state_->frozen(); }

std::shared_ptr<const ServiceRegistryState> ServiceRegistry::backing() const { return state_; }

}  // namespace rodaq::plugin
