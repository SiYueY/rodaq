#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "loader.hpp"

#include <rodaq/plugin/manager.hpp>

namespace rodaq::plugin {

struct InstanceState;
struct ManagerState;

enum class WorkStage {
    Pending,
    NativeCreate,
    FoundationValidation,
    NativeConfigure,
    OperationFreeze,
    OpenCommit,
    InitializationCleanup,
    Complete
};

enum class CloseStage {
    None,
    Pending,
    NativeShutdown,
    CallDrain,
    Destroy,
    BackingRelease,
    TerminalPublication
};

struct LifecycleWork {
    WorkStage stage{WorkStage::Pending};
    std::chrono::steady_clock::time_point stage_started{std::chrono::steady_clock::now()};
    std::weak_ptr<InstanceState> instance;
    bool done{false};
    bool submitted{false};
    bool reported{false};
    Status result;
};

struct LoadSlot {
    bool in_flight{false};
    bool loaded{false};
    Status last_result;
    std::shared_ptr<LibraryState> library;
};

struct DiagnosticEvent {
    std::array<char, 640> message{};
    std::size_t size{0};
};

struct InstanceState : std::enable_shared_from_this<InstanceState> {
    InstanceState(
        const std::shared_ptr<ManagerState>& manager, std::shared_ptr<LibraryState> library,
        std::string config, std::size_t slot, PluginInstanceId id);

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::weak_ptr<ManagerState> manager;
    ManagerState* manager_identity;
    std::shared_ptr<LibraryState> library;
    rodaq_application_callbacks_t application_callbacks{};
    std::string config;
    const std::size_t slot;
    const PluginInstanceId id;
    PluginInstancePhase phase{PluginInstancePhase::Initializing};
    rodaq_plugin_instance_t native{};
    rodaq_plugin_instance_vtable_t functions{};
    LifecycleWork initialization{};
    std::size_t active_calls{0};
    CloseStage close_stage{CloseStage::None};
    std::chrono::steady_clock::time_point close_stage_started{std::chrono::steady_clock::now()};
    std::int64_t native_shutdown_deadline{0};
    std::atomic<std::int64_t> native_shutdown_returned{-1};
    bool close_running{false};
    bool terminal{false};
    std::uint32_t reported_close_stages{0};
    Status close_result;
};

class AuthorityLease {
public:
    explicit AuthorityLease(std::uint64_t token) noexcept : token_(token) {}
    AuthorityLease(const AuthorityLease&) = delete;
    AuthorityLease& operator=(const AuthorityLease&) = delete;
    ~AuthorityLease() noexcept;

private:
    std::uint64_t token_;
};

/** Bounded runtime control domains for third-party Plugin Instances:
 * - Two lifecycle workers run Instance create/configure supervision.
 * - Four close workers run Instance shutdown/drain/destroy supervision.
 * - Monitor supervises Instance Initialize/Close runtime timeouts only (not
 *   startup Catalog scan or dlopen, which are trusted deployment bootstrap).
 */
struct ManagerState {
    ManagerState(
        PluginManagerOptions value, std::vector<CatalogEntry> discovered,
        std::uint64_t authority_token);
    ~ManagerState() noexcept;

    // Declared first, destroyed last: authority is released after all backing.
    AuthorityLease authority;
    std::shared_ptr<ManagerState> self_retention;
    mutable std::mutex mutex;
    std::condition_variable changed;
    PluginManagerOptions options;
    PluginManagerPhase phase{PluginManagerPhase::Running};
    bool restart_required{false};
    bool shutdown_requested{false};
    bool stopping{false};
    bool monitor_stopping{false};
    bool diagnostics_stopping{false};
    bool startup_aborted{false};
    bool reap_requested{false};
    bool reap_done{false};
    bool reap_reported{false};
    bool diagnostic_busy{false};
    bool diagnostic_reported{false};
    std::chrono::steady_clock::time_point reap_started{};
    std::chrono::steady_clock::time_point diagnostic_started{};
    bool launch_complete{false};
    Status failure;
    std::atomic<bool> call_violation{false};
    std::vector<CatalogEntry> catalog;
    std::vector<LoadSlot> load_slots;
    std::vector<std::shared_ptr<InstanceState>> instances;
    std::vector<bool> reserved_instances;
    std::vector<std::chrono::steady_clock::time_point> instance_reserved_at;
    std::vector<LifecycleWork*> queue;
    std::size_t queue_head{0};
    std::size_t queue_count{0};
    std::size_t instance_count{0};
    std::size_t loaded_count{0};
    std::size_t active_loads{0};
    std::size_t active_creates{0};
    std::size_t close_cursor{0};
    std::size_t close_busy{0};
    std::size_t pending_closes{0};
    std::array<DiagnosticEvent, 64> diagnostics{};
    std::size_t diagnostic_head{0};
    std::size_t diagnostic_count{0};
    std::uint64_t diagnostic_dropped{0};
    std::size_t workers_expected{0};
    std::size_t workers_exited{0};
    std::array<std::thread, 2> lifecycle_workers;
    std::array<std::thread, 4> close_workers;
    std::thread monitor;
    std::thread diagnostic_worker;
    std::thread reaper;
    std::mutex completion_mutex;
};

// All *_locked functions require ManagerState::mutex; nested order is Manager -> Instance.
void degrade_locked(ManagerState& manager, Status failure);
void request_shutdown(const std::shared_ptr<ManagerState>& manager) noexcept;
Result<void> start_workers(const std::shared_ptr<ManagerState>& manager);
bool enqueue_locked(ManagerState& manager, LifecycleWork& work) noexcept;
void request_close_locked(InstanceState& instance) noexcept;
bool current_invocation(const InstanceState& instance) noexcept;
bool current_manager_invocation(const ManagerState& manager) noexcept;
std::int64_t monotonic_now() noexcept;
Result<std::uint64_t> reserve_authority();
void release_authority(std::uint64_t token) noexcept;
void enqueue_diagnostic(
    const std::shared_ptr<ManagerState>& manager, std::string_view message,
    const PluginInfo* info = nullptr, PluginInstanceId instance = 0,
    std::string_view stage = {}) noexcept;
void run_initialization(
    const std::shared_ptr<ManagerState>& manager, const std::shared_ptr<InstanceState>& instance);
void fail_initialization(
    const std::shared_ptr<ManagerState>& manager, const std::shared_ptr<InstanceState>& instance,
    Status failure, bool contract_violation);
void run_close(
    const std::shared_ptr<ManagerState>& manager, const std::shared_ptr<InstanceState>& instance);

class LifecycleInvocation {
public:
    LifecycleInvocation(ManagerState& manager, InstanceState* instance) noexcept;
    LifecycleInvocation(const LifecycleInvocation&) = delete;
    LifecycleInvocation& operator=(const LifecycleInvocation&) = delete;
    ~LifecycleInvocation();

private:
    ManagerState* previous_manager_;
    InstanceState* previous_instance_;
};

}  // namespace rodaq::plugin
