#pragma once

#include "catalog.hpp"
#include "library_state.hpp"
#include "lifecycle_executor.hpp"
#include "lifecycle_policy.hpp"
#include "service_registry_state.hpp"

#include <rodaq/plugin/manager.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rodaq::plugin {

enum class CloseStage {
    None,
    NativeShutdown,
    CallDrain,
    Destroy,
    BackingRelease,
    TerminalPublication
};

class PluginManagerState;

class PluginInstanceState final : public std::enable_shared_from_this<PluginInstanceState> {
public:
    PluginInstanceState(
        PluginInstanceId id, std::shared_ptr<PluginLibraryState> library,
        std::shared_ptr<const ServiceRegistryState> services,
        std::weak_ptr<PluginManagerState> manager);

    [[nodiscard]] Result<void> acquire() noexcept;
    void release() noexcept;

    [[nodiscard]] PluginInstancePhase phase() const noexcept;
    [[nodiscard]] PluginInstanceId id() const noexcept { return id_; }
    [[nodiscard]] const std::shared_ptr<PluginLibraryState>& library() const noexcept {
        return library_;
    }
    [[nodiscard]] rodaq_plugin_instance_t raw_instance() const noexcept { return raw_instance_; }
    void set_raw_instance(rodaq_plugin_instance_t instance) noexcept { raw_instance_ = instance; }

    [[nodiscard]] Result<PluginInstance::QueryAdmission> query(
        std::string_view interface_id, std::uint16_t major, std::uint16_t min_minor,
        std::uint16_t max_minor);
    void record_contract_violation(StatusCode code, std::string_view message) noexcept;

    [[nodiscard]] CloseFuture request_close() noexcept;
    [[nodiscard]] bool open_for_use() noexcept;
    [[nodiscard]] bool try_begin_close() noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point close_stage_started() const noexcept;
    [[nodiscard]] CloseStage close_stage() const noexcept;
    void set_close_stage(CloseStage stage) noexcept;
    void complete_close(Result<void> result) noexcept;
    void mark_unresolved(Result<void> result) noexcept;
    void mark_initialization_closed() noexcept;

    [[nodiscard]] bool close_ready() const noexcept;
    [[nodiscard]] CloseWaitStatus wait(Deadline deadline) const noexcept;
    [[nodiscard]] Result<void> close_result() const;

    void run_close(DeadlineDuration native_timeout, std::weak_ptr<PluginManagerState> manager);

private:
    [[nodiscard]] std::int64_t now_nanoseconds() const noexcept;

    PluginInstanceId id_;
    std::shared_ptr<PluginLibraryState> library_;
    std::shared_ptr<const ServiceRegistryState> services_;
    std::weak_ptr<PluginManagerState> manager_;
    rodaq_plugin_instance_t raw_instance_{};

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    PluginInstancePhase phase_ = PluginInstancePhase::Initializing;
    std::size_t active_call_count_ = 0u;
    bool close_worker_active_ = false;
    bool close_ready_ = false;
    Result<void> close_result_;
    CloseStage close_stage_ = CloseStage::None;
    std::chrono::steady_clock::time_point close_stage_started_{};
};

class PluginManagerState final : public std::enable_shared_from_this<PluginManagerState> {
public:
    explicit PluginManagerState(
        PluginManagerOptions options, LifecyclePolicy lifecycle_policy = {});
    PluginManagerState(const PluginManagerState&) = delete;
    PluginManagerState& operator=(const PluginManagerState&) = delete;
    ~PluginManagerState() = default;

    void start_workers();

    [[nodiscard]] Result<void> refresh();
    [[nodiscard]] Result<std::vector<PluginInfo>> plugins() const;
    [[nodiscard]] Result<std::shared_ptr<PluginLibraryState>> load(
        std::string_view plugin_id, std::string_view plugin_version, bool exact_version);
    [[nodiscard]] Result<PluginDiagnostics> diagnostics() const;
    [[nodiscard]] Result<void> shutdown(Deadline deadline);
    void begin_shutdown() noexcept;

    [[nodiscard]] Result<std::shared_ptr<PluginInstanceState>> create_instance(
        const std::shared_ptr<PluginLibraryState>& library,
        const std::shared_ptr<const ServiceRegistryState>& services, std::string_view config);

    void request_close(const std::shared_ptr<PluginInstanceState>& instance) noexcept;
    void finish_closed_instance(PluginInstanceId id) noexcept;
    void mark_library_faulted(const std::shared_ptr<PluginLibraryState>& library) noexcept;
    void mark_degraded(StatusCode code, std::string_view message) noexcept;
    [[nodiscard]] PluginManagerPhase phase() const noexcept;
    [[nodiscard]] bool restart_required() const noexcept;

private:
    void close_worker_loop();
    void supervision_loop();
    void add_diagnostic(
        std::string plugin_id, std::string plugin_version, PluginInstanceId instance_id,
        StatusCode code, std::string reason, std::string message);
    [[nodiscard]] bool success_predicate_locked() const noexcept;
    [[nodiscard]] bool has_closing_instance_locked() const;
    [[nodiscard]] std::shared_ptr<PluginInstanceState> find_ready_to_close_locked();
    void signal_lock() noexcept;
    [[nodiscard]] std::shared_ptr<LifecycleWorkState> begin_lifecycle_work(LifecycleWorkKind kind);
    void finish_lifecycle_work(const std::shared_ptr<LifecycleWorkState>& work);

    PluginManagerOptions options_;
    LifecyclePolicy lifecycle_policy_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<PluginManagerPhase> phase_{PluginManagerPhase::Running};
    bool restart_required_ = false;
    bool teardown_started_ = false;
    bool teardown_complete_ = false;
    std::shared_ptr<CatalogGeneration> catalog_;
    std::unordered_map<std::string, std::shared_ptr<PluginLibraryState>> loaded_plugins_;
    std::unordered_map<PluginInstanceId, std::shared_ptr<PluginInstanceState>> instances_;
    std::unordered_map<std::string, std::shared_future<Result<std::shared_ptr<PluginLibraryState>>>>
        load_operations_;
    std::shared_future<Result<std::shared_ptr<CatalogGeneration>>> refresh_operation_;
    bool refresh_active_ = false;
    std::size_t active_refresh_count_ = 0u;
    std::size_t active_load_count_ = 0u;
    std::size_t active_create_count_ = 0u;
    PluginInstanceId next_lifecycle_work_id_ = 1u;
    std::unordered_map<PluginInstanceId, std::shared_ptr<LifecycleWorkState>> lifecycle_work_;
    PluginInstanceId next_instance_id_ = 1u;
    LifecycleExecutor lifecycle_executor_;
    std::vector<std::thread> close_workers_;
    std::thread supervision_worker_;
    std::atomic<bool> stop_workers_{false};
    std::deque<DiagnosticRecord> diagnostics_;
};

}  // namespace rodaq::plugin
