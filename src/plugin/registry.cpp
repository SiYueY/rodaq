#include <rodaq/plugin/registry.hpp>

#include "registry_state.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace rodaq::plugin {

RegistryState::RegistryState() {
    entries.reserve(authoring::kMaxInterfaces);
    owners.reserve(authoring::kMaxInterfaces);
}

ServiceRegistry::ServiceRegistry() : state_(std::make_shared<RegistryState>()) {}

ServiceRegistry::ServiceRegistry(ServiceRegistry&&) noexcept = default;

ServiceRegistry& ServiceRegistry::operator=(ServiceRegistry&&) noexcept = default;

ServiceRegistry::~ServiceRegistry() noexcept = default;

Result<void> ServiceRegistry::add_binding(
    authoring::InterfaceEntry binding, std::shared_ptr<void> owner) {
    if (!state_) {
        return Status(StatusCode::InvalidState, "Moved-from ServiceRegistry");
    }
    // Parameters (including a rejected provider's owner) release after this lock.
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->frozen.load(std::memory_order_relaxed)) {
        return Status(StatusCode::InvalidState, "ServiceRegistry is frozen");
    }
    const auto duplicate =
        std::find_if(state_->entries.begin(), state_->entries.end(), [&](const auto& entry) {
            return entry.id == binding.id && entry.reference.negotiated_version.major ==
                                                 binding.reference.negotiated_version.major;
        });
    if (duplicate != state_->entries.end()) {
        return Status(StatusCode::Conflict, "Duplicate Service ID/major");
    }
    if (state_->entries.size() >= authoring::kMaxInterfaces) {
        return Status(StatusCode::ResourceExhausted, "ServiceRegistry capacity");
    }
    // Both arrays were reserved before any provider was accepted; these moves
    // neither allocate nor invoke provider code while the structural lock is held.
    state_->entries.push_back(std::move(binding));
    state_->owners.push_back(std::move(owner));
    return {};
}

Result<void> ServiceRegistry::freeze() {
    if (!state_) {
        return Status(StatusCode::InvalidState, "Moved-from ServiceRegistry");
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->frozen.store(true, std::memory_order_release);
    return {};
}

bool ServiceRegistry::frozen() const noexcept {
    return state_ && state_->frozen.load(std::memory_order_acquire);
}

rodaq_status_code_t registry_query(
    const RegistryState& registry, const rodaq_interface_query_t* query,
    rodaq_interface_reference_t* reference, rodaq_error_t* error) noexcept {
    if (!registry.frozen.load(std::memory_order_acquire)) {
        if (reference != nullptr) {
            *reference = {};
        }
        abi::write_error(error, {});
        return RODAQ_STATUS_INVALID_STATE;
    }
    return authoring::query_interface(registry.entries, query, reference, error);
}

}  // namespace rodaq::plugin
