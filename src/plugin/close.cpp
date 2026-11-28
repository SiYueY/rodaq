#include "lifecycle.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include <rodaq/plugin/abi.hpp>

namespace rodaq::plugin {
namespace {

void close_stage(InstanceState& instance, CloseStage stage) {
    std::lock_guard<std::mutex> lock(instance.mutex);
    instance.close_stage = stage;
    instance.close_stage_started = std::chrono::steady_clock::now();
}

void unresolved(
    const std::shared_ptr<ManagerState>& manager, const std::shared_ptr<InstanceState>& instance,
    Status failure) {
    {
        std::lock_guard<std::mutex> manager_lock(manager->mutex);
        std::lock_guard<std::mutex> instance_lock(instance->mutex);
        degrade_locked(*manager, Status(failure.code(), {}));
        instance->close_result = std::move(failure);
        instance->phase = PluginInstancePhase::Unresolved;
        instance->terminal = true;
        --manager->close_busy;
    }
    instance->changed.notify_all();
    manager->changed.notify_all();
    enqueue_diagnostic(
        manager, "Plugin shutdown failed; Instance ownership retained", &instance->library->info,
        instance->id, "shutdown");
}

}  // namespace

void run_close(
    const std::shared_ptr<ManagerState>& manager, const std::shared_ptr<InstanceState>& instance) {
    auto deadline = Deadline::after(manager->options.shutdown_timeout);
    if (!deadline.ok()) {
        unresolved(manager, instance, std::move(deadline).status());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(instance->mutex);
        instance->close_stage = CloseStage::NativeShutdown;
        instance->close_stage_started = std::chrono::steady_clock::now();
        instance->native_shutdown_deadline = deadline.value().native().monotonic_deadline;
        instance->native_shutdown_returned.store(-1, std::memory_order_release);
    }
    std::array<std::uint8_t, 512> buffer{};
    rodaq_error_t error{{buffer.data(), buffer.size(), 0}};
    const abi::BufferSnapshot expected{buffer.data(), buffer.size()};
    const auto code =
        instance->functions.shutdown(instance->native.context, deadline.value().native(), &error);
    const auto returned = monotonic_now();
    instance->native_shutdown_returned.store(returned, std::memory_order_release);
    auto status = abi::status_from_abi(code, error, expected);
    if (!abi::known_status(code) || !abi::valid_error(code, error, expected)) {
        enqueue_diagnostic(
            manager, "Plugin shutdown status/error ABI violation", &instance->library->info,
            instance->id, "shutdown");
    }
    if (!status.ok()) {
        unresolved(manager, instance, std::move(status));
        return;
    }
    if (returned < 0 || returned > deadline.value().native().monotonic_deadline) {
        {
            std::lock_guard<std::mutex> lock(manager->mutex);
            degrade_locked(*manager, Status(StatusCode::DeadlineExceeded, {}));
        }
        manager->changed.notify_all();
    }

    close_stage(*instance, CloseStage::CallDrain);
    {
        std::unique_lock<std::mutex> lock(instance->mutex);
        instance->changed.wait(lock, [&] { return instance->active_calls == 0; });
    }
    // Closing admission is irreversible, so zero remains stable after drain.
    close_stage(*instance, CloseStage::Destroy);
    instance->functions.destroy(instance->native.context);
    instance->native = {};
    instance->functions = {};

    close_stage(*instance, CloseStage::BackingRelease);
    instance->application_callbacks = {};
    close_stage(*instance, CloseStage::TerminalPublication);
    {
        std::lock_guard<std::mutex> manager_lock(manager->mutex);
        std::lock_guard<std::mutex> instance_lock(instance->mutex);
        manager->instances[instance->slot].reset();
        manager->reserved_instances[instance->slot] = false;
        --manager->instance_count;
        --manager->close_busy;
        instance->close_result = Status{};
        instance->phase = PluginInstancePhase::Closed;
        instance->terminal = true;
    }
    // Capacity, actual destroy and backing release precede this sole publication.
    instance->changed.notify_all();
    manager->changed.notify_all();
}

}  // namespace rodaq::plugin
