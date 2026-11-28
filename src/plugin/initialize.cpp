#include "lifecycle.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#include <rodaq/plugin/abi.hpp>

namespace rodaq::plugin {
namespace {

void stage(const std::shared_ptr<ManagerState>& manager, InstanceState& instance, WorkStage value) {
    std::lock_guard<std::mutex> lock(manager->mutex);
    instance.initialization.stage = value;
    instance.initialization.stage_started = std::chrono::steady_clock::now();
}

bool valid_foundation(InstanceState& instance) noexcept {
    const auto& native = instance.native;
    constexpr auto kRequired = offsetof(rodaq_plugin_instance_vtable_t, destroy) +
                               sizeof(rodaq_plugin_instance_vtable_t::destroy);
    if (native.context == nullptr || native.vtable == nullptr ||
        reinterpret_cast<std::uintptr_t>(native.vtable) % alignof(rodaq_plugin_instance_vtable_t) !=
            0) {
        return false;
    }
    rodaq_struct_header_t header{};
    std::memcpy(&header, native.vtable, sizeof(header));
    if (header.struct_size < kRequired) {
        return false;
    }
    std::memcpy(&instance.functions, native.vtable, kRequired);
    return instance.functions.configure != nullptr && instance.functions.query != nullptr &&
           instance.functions.shutdown != nullptr && instance.functions.destroy != nullptr;
}

}  // namespace

void fail_initialization(
    const std::shared_ptr<ManagerState>& manager, const std::shared_ptr<InstanceState>& instance,
    Status failure, bool contract_violation) {
    stage(manager, *instance, WorkStage::InitializationCleanup);
    const bool owned_native =
        instance->functions.configure != nullptr && instance->functions.query != nullptr &&
        instance->functions.shutdown != nullptr && instance->functions.destroy != nullptr &&
        instance->native.context != nullptr;
    if (!owned_native) {
        // No config was given to unvalidated create output. Drop only Framework-owned
        // input backing; never guess how to destroy that output.
        std::string{}.swap(instance->config);
    }
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        if (contract_violation) {
            instance->library->faulted = true;
            degrade_locked(*manager, Status(StatusCode::Internal, {}));
        }
        if (owned_native) {
            request_close_locked(*instance);
        } else if (contract_violation) {
            // Retain even uninterpretable output in its reserved State. There is
            // no validated cleanup authority, and dropping the opaque context
            // would lose native backing before the required process restart.
            std::lock_guard<std::mutex> state_lock(instance->mutex);
            instance->phase = PluginInstancePhase::Unresolved;
            instance->terminal = true;
            instance->close_result = Status(StatusCode::Internal, {});
        } else {
            manager->instances[instance->slot].reset();
            manager->reserved_instances[instance->slot] = false;
            --manager->instance_count;
        }
        --manager->active_creates;
        instance->initialization.result = std::move(failure);
        instance->initialization.done = true;
        instance->initialization.stage = WorkStage::Complete;
    }
    manager->changed.notify_all();
    if (contract_violation) {
        enqueue_diagnostic(
            manager, "Plugin initialization ABI violation; restart required",
            &instance->library->info, instance->id, "initialize");
    }
}

void run_initialization(
    const std::shared_ptr<ManagerState>& manager, const std::shared_ptr<InstanceState>& instance) {
    bool admitted = false;
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        admitted = manager->phase == PluginManagerPhase::Running && !instance->library->faulted;
    }
    if (!admitted) {
        fail_initialization(
            manager, instance, Status(StatusCode::InvalidState, "Manager stopped before create"),
            false);
        return;
    }
    stage(manager, *instance, WorkStage::NativeCreate);
    std::array<std::uint8_t, 512> buffer{};
    rodaq_error_t error{{buffer.data(), buffer.size(), 0}};
    const abi::BufferSnapshot expected{buffer.data(), buffer.size()};
    instance->native = {};
    const auto created = instance->library->entry.create(&instance->native, &error);
    stage(manager, *instance, WorkStage::FoundationValidation);
    auto create_status = abi::status_from_abi(created, error, expected);
    const bool empty = instance->native.context == nullptr && instance->native.vtable == nullptr;
    const bool malformed =
        !abi::known_status(created) ||
        (created == RODAQ_STATUS_OK &&
         (!abi::valid_error(created, error, expected) || !valid_foundation(*instance))) ||
        (created != RODAQ_STATUS_OK && !empty);
    if (malformed) {
        // Even a superficially readable vtable is discarded when create itself
        // violated ownership/status. There is no validated cleanup authority.
        instance->functions = {};
        fail_initialization(
            manager, instance, Status(StatusCode::Internal, "Malformed Plugin create output"),
            true);
        return;
    }
    if (!create_status.ok()) {
        fail_initialization(manager, instance, std::move(create_status), false);
        return;
    }
    stage(manager, *instance, WorkStage::NativeConfigure);
    error = {{buffer.data(), buffer.size(), 0}};
    const rodaq_config_t configuration{
        {sizeof(rodaq_config_t)},
        {instance->config.empty() ? nullptr : instance->config.data(),
         static_cast<std::uint32_t>(instance->config.size())}};
    const auto configured = instance->functions.configure(
        instance->native.context, &instance->application_callbacks, &configuration, &error);
    stage(manager, *instance, WorkStage::OperationFreeze);
    auto configure_status = abi::status_from_abi(configured, error, expected);
    const bool configure_violation =
        !abi::known_status(configured) ||
        (configured == RODAQ_STATUS_OK && !abi::valid_error(configured, error, expected));
    // Borrowed JSON lives through actual configure return, including late return.
    std::string{}.swap(instance->config);
    if (!configure_status.ok()) {
        fail_initialization(manager, instance, std::move(configure_status), configure_violation);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        instance->initialization.stage = WorkStage::OpenCommit;
        instance->initialization.stage_started = std::chrono::steady_clock::now();
        if (manager->phase == PluginManagerPhase::Running && !instance->library->faulted) {
            std::lock_guard<std::mutex> state_lock(instance->mutex);
            instance->phase = PluginInstancePhase::Open;
        } else {
            request_close_locked(*instance);
            instance->initialization.result =
                Status(StatusCode::InvalidState, "Manager won initialization race");
        }
        --manager->active_creates;
        instance->initialization.done = true;
        instance->initialization.stage = WorkStage::Complete;
    }
    manager->changed.notify_all();
}

}  // namespace rodaq::plugin
