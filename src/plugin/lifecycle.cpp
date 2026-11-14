#include "lifecycle.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

#include <rodaq/logging.hpp>
#include <rodaq/plugin/abi.hpp>

namespace rodaq::plugin {
namespace {

constexpr auto kStageTimeout = std::chrono::seconds(30);
constexpr std::size_t kMaxLoadObservers = 1024;

struct CreationAuthority {
    std::mutex mutex;
    std::uint64_t token{0};
    bool occupied{false};
};

CreationAuthority& creation_authority() {
    static auto* const authority = new CreationAuthority;
    return *authority;
}

struct HealthEvent {
    std::array<char, 256> plugin{};
    std::array<char, 129> version{};
    PluginInstanceId instance{0};
    int stage{0};
    bool close{false};
};

void fill_event(
    HealthEvent& event, const PluginInfo& info, PluginInstanceId id, int stage,
    bool close) noexcept {
    std::memcpy(
        event.plugin.data(), info.plugin_id.data(),
        std::min(info.plugin_id.size(), event.plugin.size() - 1));
    std::memcpy(
        event.version.data(), info.plugin_version.data(),
        std::min(info.plugin_version.size(), event.version.size() - 1));
    event.instance = id;
    event.stage = stage;
    event.close = close;
}

bool shutdown_settled_locked(const ManagerState& manager) noexcept {
    return manager.shutdown_requested && !manager.restart_required && manager.discovery.done &&
           manager.active_loads == 0 && manager.active_creates == 0 && manager.queue_count == 0 &&
           manager.instance_count == 0 && manager.pending_closes == 0 && manager.close_busy == 0;
}

void worker_exited(const std::shared_ptr<ManagerState>& manager) noexcept {
    std::shared_ptr<ManagerState> retention;
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        ++manager->workers_exited;
        if (manager->launch_complete && manager->stopping &&
            manager->workers_exited == manager->workers_expected && !manager->restart_required) {
            manager->phase = PluginManagerPhase::Stopped;
            retention = std::move(manager->self_retention);
        }
    }
    manager->changed.notify_all();
    // The worker's own strong reference still pins State while retention drops.
    retention.reset();
}

void failed_work(
    const std::shared_ptr<ManagerState>& manager, LifecycleWork& work,
    const std::shared_ptr<InstanceState>& instance, StatusCode code, bool critical) {
    if (work.kind == WorkKind::Initialize && instance) {
        fail_initialization(manager, instance, Status(code, {}), critical);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        if (critical) {
            degrade_locked(*manager, Status(code, {}));
        }
        if (work.kind == WorkKind::Load && !work.done) {
            --manager->active_loads;
            --manager->loaded_count;
        }
        work.result = Status(code, {});
        work.done = true;
        work.stage = WorkStage::Complete;
    }
    manager->changed.notify_all();
}

void lifecycle_worker(const std::shared_ptr<ManagerState>& manager) noexcept {
    for (;;) {
        LifecycleWork* work = nullptr;
        std::shared_ptr<LifecycleWork> load_owner;
        std::shared_ptr<InstanceState> instance;
        {
            std::unique_lock<std::mutex> lock(manager->mutex);
            manager->changed.wait(
                lock, [&] { return manager->stopping || manager->queue_count != 0; });
            if (manager->stopping) {
                break;
            }
            work = manager->queue[manager->queue_head];
            manager->queue_head = (manager->queue_head + 1) % manager->queue.size();
            --manager->queue_count;
            if (work->kind == WorkKind::Load) {
                load_owner = work->shared_from_this();
            } else if (work->kind == WorkKind::Initialize) {
                instance = work->instance.lock();
            }
        }
        LifecycleInvocation invocation(*manager, instance.get());
        try {
            switch (work->kind) {
                case WorkKind::Catalog:
                    run_discovery(manager);
                    break;
                case WorkKind::Load:
                    run_load(manager, *work);
                    break;
                case WorkKind::Initialize:
                    if (!instance) {
                        throw std::bad_alloc();
                    }
                    run_initialization(manager, instance);
                    break;
            }
        } catch (const std::bad_alloc&) {
            const bool native =
                work->kind == WorkKind::Initialize && (work->stage == WorkStage::NativeCreate ||
                                                       work->stage == WorkStage::NativeConfigure);
            failed_work(
                manager, *work, instance,
                native ? StatusCode::Internal : StatusCode::ResourceExhausted, native);
        } catch (...) {
            failed_work(manager, *work, instance, StatusCode::Internal, true);
            if (work->kind == WorkKind::Load) {
                enqueue_diagnostic(
                    manager, "Exception crossed load ABI; restart required",
                    &manager->catalog[work->catalog_index].info, 0, "native-load");
            }
        }
    }
    worker_exited(manager);
}

std::shared_ptr<InstanceState> take_close_locked(ManagerState& manager) {
    for (std::size_t offset = 0; offset < manager.instances.size(); ++offset) {
        const auto index = (manager.close_cursor + offset) % manager.instances.size();
        const auto& instance = manager.instances[index];
        if (!instance) {
            continue;
        }
        std::lock_guard<std::mutex> lock(instance->mutex);
        if (instance->phase == PluginInstancePhase::Closing && !instance->close_running) {
            instance->close_running = true;
            manager.close_cursor = (index + 1) % manager.instances.size();
            --manager.pending_closes;
            ++manager.close_busy;
            return instance;
        }
    }
    manager.call_violation.store(true);
    return {};
}

void close_worker(const std::shared_ptr<ManagerState>& manager) noexcept {
    for (;;) {
        std::shared_ptr<InstanceState> instance;
        {
            std::unique_lock<std::mutex> lock(manager->mutex);
            manager->changed.wait(
                lock, [&] { return manager->stopping || manager->pending_closes != 0; });
            if (manager->stopping) {
                break;
            }
            instance = take_close_locked(*manager);
        }
        if (!instance) {
            continue;
        }
        LifecycleInvocation invocation(*manager, instance.get());
        try {
            run_close(manager, instance);
        } catch (...) {
            {
                std::lock_guard<std::mutex> manager_lock(manager->mutex);
                std::lock_guard<std::mutex> instance_lock(instance->mutex);
                degrade_locked(*manager, Status(StatusCode::Internal, {}));
                instance->phase = PluginInstancePhase::Unresolved;
                instance->close_result = Status(StatusCode::Internal, {});
                instance->terminal = true;
                --manager->close_busy;
            }
            instance->changed.notify_all();
            manager->changed.notify_all();
            enqueue_diagnostic(
                manager, "Plugin close exception; owner retained", &instance->library->info,
                instance->id, "close");
        }
    }
    worker_exited(manager);
}

void diagnostic_worker(const std::shared_ptr<ManagerState>& manager) noexcept {
    for (;;) {
        DiagnosticEvent event;
        {
            std::unique_lock<std::mutex> lock(manager->mutex);
            manager->changed.wait(lock, [&] {
                return manager->diagnostics_stopping || manager->diagnostic_count != 0;
            });
            if (manager->diagnostic_count == 0 && manager->diagnostics_stopping) {
                break;
            }
            event = manager->diagnostics[manager->diagnostic_head];
            manager->diagnostic_head = (manager->diagnostic_head + 1) % manager->diagnostics.size();
            --manager->diagnostic_count;
            manager->diagnostic_busy = true;
            manager->diagnostic_reported = false;
            manager->diagnostic_started = std::chrono::steady_clock::now();
        }
        logging::write(logging::Level::Critical, {}, {event.message.data(), event.size});
        {
            std::lock_guard<std::mutex> lock(manager->mutex);
            manager->diagnostic_busy = false;
        }
        manager->changed.notify_all();
    }
    worker_exited(manager);
}

void reaper_worker(const std::shared_ptr<ManagerState>& manager) noexcept {
    {
        std::unique_lock<std::mutex> lock(manager->mutex);
        manager->changed.wait(lock, [&] {
            return manager->startup_aborted ||
                   (manager->launch_complete && manager->reap_requested);
        });
        if (manager->startup_aborted) {
            lock.unlock();
            worker_exited(manager);
            return;
        }
    }
    // Joining may run/wait for dependency TLS cleanup. Health monitor remains
    // independent and supervises this whole retirement stage.
    for (auto& worker : manager->lifecycle_workers) {
        worker.join();
    }
    for (auto& worker : manager->close_workers) {
        worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        manager->diagnostics_stopping = true;
    }
    manager->changed.notify_all();
    manager->diagnostic_worker.join();
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        manager->reap_done = true;
    }
    manager->changed.notify_all();
    worker_exited(manager);
}

std::chrono::nanoseconds pending_timeout(const ManagerState& manager) noexcept {
    constexpr auto kBase =
        std::chrono::duration_cast<std::chrono::nanoseconds>(kStageTimeout).count();
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    const auto batches = (manager.options.max_instances + manager.close_workers.size() - 1) /
                         manager.close_workers.size();
    const auto policy = manager.options.shutdown_timeout.count();
    if (policy > (kMax - kBase) / static_cast<std::int64_t>(batches)) {
        return std::chrono::nanoseconds::max();
    }
    return std::chrono::nanoseconds(kBase + policy * static_cast<std::int64_t>(batches));
}

void monitor_worker(const std::shared_ptr<ManagerState>& manager) noexcept {
    for (;;) {
        std::array<HealthEvent, 8> events{};
        std::size_t event_count = 0;
        const auto now = std::chrono::steady_clock::now();
        const auto native_now = monotonic_now();
        const PluginInfo empty_info{};
        bool notify = false;
        bool join_reaper = false;
        {
            std::unique_lock<std::mutex> lock(manager->mutex);
            if (manager->monitor_stopping) {
                break;
            }
            if (manager->call_violation.exchange(false)) {
                degrade_locked(*manager, Status(StatusCode::Internal, {}));
                notify = true;
            }
            auto inspect = [&](LifecycleWork& work, const PluginInfo& info, PluginInstanceId id) {
                if (work.submitted && !work.done && !work.reported &&
                    now - work.stage_started > kStageTimeout && event_count < events.size()) {
                    work.reported = true;
                    fill_event(
                        events[event_count++], info, id, static_cast<int>(work.stage), false);
                    degrade_locked(*manager, Status(StatusCode::DeadlineExceeded, {}));
                }
            };
            inspect(manager->discovery, empty_info, 0);
            for (const auto& work : manager->load_pool) {
                inspect(
                    *work,
                    work->catalog_index < manager->catalog.size()
                        ? manager->catalog[work->catalog_index].info
                        : empty_info,
                    0);
            }
            for (std::size_t i = 0; i < manager->instances.size(); ++i) {
                const auto& instance = manager->instances[i];
                if (!instance) {
                    if (manager->reserved_instances[i] &&
                        now - manager->instance_reserved_at[i] > kStageTimeout) {
                        degrade_locked(*manager, Status(StatusCode::DeadlineExceeded, {}));
                        notify = true;
                    }
                    continue;
                }
                inspect(instance->initialization, instance->library->info, instance->id);
                std::lock_guard<std::mutex> state_lock(instance->mutex);
                if (instance->phase != PluginInstancePhase::Closing) {
                    continue;
                }
                const auto elapsed = now - instance->close_stage_started;
                bool expired = elapsed > (instance->close_stage == CloseStage::Pending
                                              ? pending_timeout(*manager)
                                              : kStageTimeout);
                if (instance->close_stage == CloseStage::NativeShutdown) {
                    const auto returned =
                        instance->native_shutdown_returned.load(std::memory_order_acquire);
                    expired = expired ||
                              (native_now >= instance->native_shutdown_deadline && returned < 0) ||
                              returned > instance->native_shutdown_deadline;
                }
                const auto bit = std::uint32_t{1}
                                 << static_cast<unsigned int>(instance->close_stage);
                if (expired && (instance->reported_close_stages & bit) == 0 &&
                    event_count < events.size()) {
                    instance->reported_close_stages |= bit;
                    fill_event(
                        events[event_count++], instance->library->info, instance->id,
                        static_cast<int>(instance->close_stage), true);
                    degrade_locked(*manager, Status(StatusCode::DeadlineExceeded, {}));
                }
            }
            if (manager->diagnostic_busy && !manager->diagnostic_reported &&
                now - manager->diagnostic_started > kStageTimeout) {
                manager->diagnostic_reported = true;
                degrade_locked(*manager, Status(StatusCode::DeadlineExceeded, {}));
                notify = true;
            }
            if (manager->reap_requested && !manager->reap_done && !manager->reap_reported &&
                now - manager->reap_started > kStageTimeout) {
                manager->reap_reported = true;
                degrade_locked(*manager, Status(StatusCode::DeadlineExceeded, {}));
                notify = true;
            }
            if (shutdown_settled_locked(*manager) && !manager->reap_requested) {
                manager->stopping = true;
                manager->reap_requested = true;
                manager->reap_started = now;
                notify = true;
            }
            join_reaper = manager->reap_done;
            if (!join_reaper) {
                manager->changed.wait_for(lock, std::chrono::milliseconds(10));
            }
        }
        // Diagnostic formatting and sinks never run under Manager/Instance locks.
        for (std::size_t i = 0; i < event_count; ++i) {
            const auto& event = events[i];
            std::array<char, 640> message{};
            const auto size = std::snprintf(
                message.data(), message.size(),
                "Plugin lifecycle timeout plugin=%s version=%s instance=%llu domain=%s stage=%d "
                "monotonic=%lld",
                event.plugin.data(), event.version.data(),
                static_cast<unsigned long long>(event.instance),
                event.close ? "close" : "initialize/load", event.stage,
                static_cast<long long>(native_now));
            if (size > 0) {
                enqueue_diagnostic(
                    manager,
                    {message.data(),
                     std::min<std::size_t>(static_cast<std::size_t>(size), message.size() - 1)});
            }
        }
        if (notify || event_count != 0) {
            manager->changed.notify_all();
        }
        if (join_reaper) {
            manager->reaper.join();
            std::lock_guard<std::mutex> lock(manager->mutex);
            manager->monitor_stopping = true;
        }
    }
    worker_exited(manager);
}

}  // namespace

Result<std::uint64_t> reserve_authority() {
    auto& authority = creation_authority();
    std::lock_guard<std::mutex> lock(authority.mutex);
    if (authority.occupied) {
        return Status(StatusCode::Conflict, "A PluginManager authority is still live");
    }
    if (authority.token == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::ResourceExhausted, "PluginManager authority identity exhausted");
    }
    authority.occupied = true;
    return ++authority.token;
}

void release_authority(std::uint64_t token) noexcept {
    auto& authority = creation_authority();
    std::lock_guard<std::mutex> lock(authority.mutex);
    if (authority.token == token) {
        authority.occupied = false;
    }
}

AuthorityLease::~AuthorityLease() noexcept { release_authority(token_); }

ManagerState::ManagerState(PluginManagerOptions value, std::uint64_t token)
: authority(token),
  options(std::move(value)),
  instances(options.max_instances),
  reserved_instances(options.max_instances, false),
  instance_reserved_at(options.max_instances),
  queue(options.max_instances + options.max_loaded_plugins + 1, nullptr) {}

ManagerState::~ManagerState() noexcept {
    // Threads reach their exit accounting before normal self-retention releases.
    // No native work can outlive this State; detach only reclaims finished handles.
    for (auto& worker : lifecycle_workers) {
        if (worker.joinable()) {
            worker.detach();
        }
    }
    for (auto& worker : close_workers) {
        if (worker.joinable()) {
            worker.detach();
        }
    }
    if (diagnostic_worker.joinable()) {
        diagnostic_worker.detach();
    }
    if (reaper.joinable()) {
        reaper.detach();
    }
    if (monitor.joinable()) {
        monitor.detach();
    }
}

void enqueue_diagnostic(
    const std::shared_ptr<ManagerState>& manager, std::string_view message, const PluginInfo* info,
    PluginInstanceId instance, std::string_view stage) noexcept {
    if (!manager) {
        return;
    }
    std::array<char, 640> formatted{};
    if (info != nullptr) {
        const auto count = std::snprintf(
            formatted.data(), formatted.size(),
            "plugin=%s version=%s instance=%llu stage=%.*s %.*s", info->plugin_id.c_str(),
            info->plugin_version.c_str(), static_cast<unsigned long long>(instance),
            static_cast<int>(std::min(stage.size(), std::size_t{64})), stage.data(),
            static_cast<int>(std::min(message.size(), std::size_t{256})), message.data());
        if (count > 0) {
            message = {
                formatted.data(), std::min(static_cast<std::size_t>(count), formatted.size() - 1)};
        }
    }
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        if (manager->diagnostic_count == manager->diagnostics.size() ||
            manager->diagnostics_stopping) {
            if (manager->diagnostic_dropped != std::numeric_limits<std::uint64_t>::max()) {
                ++manager->diagnostic_dropped;
            }
            return;
        }
        auto& event = manager->diagnostics
                          [(manager->diagnostic_head + manager->diagnostic_count) %
                           manager->diagnostics.size()];
        event.size = abi::utf8_prefix(message, event.message.size());
        if (event.size != 0) {
            std::memcpy(event.message.data(), message.data(), event.size);
        }
        ++manager->diagnostic_count;
    }
    manager->changed.notify_all();
}

void degrade_locked(ManagerState& manager, Status failure) {
    if (!manager.restart_required) {
        manager.failure = failure.ok() ? Status(StatusCode::Internal, {}) : std::move(failure);
    }
    manager.restart_required = true;
    manager.phase = PluginManagerPhase::Degraded;
}

bool enqueue_locked(ManagerState& manager, LifecycleWork& work) noexcept {
    if (manager.queue_count == manager.queue.size()) {
        return false;
    }
    work.stage = WorkStage::Pending;
    work.stage_started = std::chrono::steady_clock::now();
    work.submitted = true;
    manager.queue[(manager.queue_head + manager.queue_count) % manager.queue.size()] = &work;
    ++manager.queue_count;
    return true;
}

void request_close_locked(InstanceState& instance) noexcept {
    std::lock_guard<std::mutex> lock(instance.mutex);
    if (instance.phase == PluginInstancePhase::Open ||
        instance.phase == PluginInstancePhase::Initializing) {
        instance.phase = PluginInstancePhase::Closing;
        instance.close_stage = CloseStage::Pending;
        instance.close_stage_started = std::chrono::steady_clock::now();
        ++instance.manager_identity->pending_closes;
    }
}

void request_shutdown(const std::shared_ptr<ManagerState>& manager) noexcept {
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        manager->shutdown_requested = true;
        if (manager->phase == PluginManagerPhase::Running) {
            manager->phase = PluginManagerPhase::ShuttingDown;
        }
        for (const auto& instance : manager->instances) {
            if (!instance) {
                continue;
            }
            bool open = false;
            {
                std::lock_guard<std::mutex> state_lock(instance->mutex);
                open = instance->phase == PluginInstancePhase::Open;
            }
            if (open) {
                request_close_locked(*instance);
            }
        }
    }
    manager->changed.notify_all();
}

Result<void> start_workers(const std::shared_ptr<ManagerState>& manager) {
    try {
        for (auto& worker : manager->lifecycle_workers) {
            worker = std::thread([manager] { lifecycle_worker(manager); });
            ++manager->workers_expected;
        }
        for (auto& worker : manager->close_workers) {
            worker = std::thread([manager] { close_worker(manager); });
            ++manager->workers_expected;
        }
        manager->diagnostic_worker = std::thread([manager] { diagnostic_worker(manager); });
        ++manager->workers_expected;
        manager->reaper = std::thread([manager] { reaper_worker(manager); });
        ++manager->workers_expected;
        manager->monitor = std::thread([manager] { monitor_worker(manager); });
        ++manager->workers_expected;
        {
            std::lock_guard<std::mutex> lock(manager->mutex);
            manager->launch_complete = true;
        }
        return {};
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(manager->mutex);
            manager->stopping = true;
            manager->startup_aborted = true;
            manager->diagnostics_stopping = true;
            manager->monitor_stopping = true;
        }
        manager->changed.notify_all();
        for (auto& worker : manager->lifecycle_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        for (auto& worker : manager->close_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        if (manager->diagnostic_worker.joinable()) {
            manager->diagnostic_worker.join();
        }
        if (manager->reaper.joinable()) {
            manager->reaper.join();
        }
        if (manager->monitor.joinable()) {
            manager->monitor.join();
        }
        manager->self_retention.reset();
        return Status(StatusCode::ResourceExhausted, "Cannot start Plugin lifecycle workers");
    }
}

void run_discovery(const std::shared_ptr<ManagerState>& manager) {
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        manager->discovery.stage = WorkStage::CatalogScan;
        manager->discovery.stage_started = std::chrono::steady_clock::now();
    }
    auto catalog = discover_plugins(manager->options.search_paths);
    std::vector<std::shared_ptr<LifecycleWork>> pool;
    std::vector<std::shared_ptr<LifecycleWork>> loads;
    if (catalog.ok()) {
        loads.resize(catalog.value().size());
        const auto capacity =
            kMaxLoadObservers + kMaxPackageEntries + manager->options.max_loaded_plugins;
        pool.reserve(capacity);
        for (std::size_t i = 0; i < capacity; ++i) {
            pool.push_back(std::make_shared<LifecycleWork>(WorkKind::Load));
        }
    }
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        if (catalog.ok() && manager->phase == PluginManagerPhase::Running) {
            manager->catalog = std::move(catalog).value();
            manager->loads = std::move(loads);
            manager->load_pool = std::move(pool);
        } else {
            manager->discovery.result =
                catalog.ok() ? Status(StatusCode::InvalidState, {}) : std::move(catalog).status();
        }
        manager->discovery.done = true;
        manager->discovery.stage = WorkStage::Complete;
    }
    manager->changed.notify_all();
}

void run_load(const std::shared_ptr<ManagerState>& manager, LifecycleWork& work) {
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        work.stage = WorkStage::NativeLoad;
        work.stage_started = std::chrono::steady_clock::now();
    }
    auto prepared = prepare_library(manager->catalog[work.catalog_index]);
    Status failure;
    if (prepared.ok()) {
        {
            std::lock_guard<std::mutex> lock(manager->mutex);
            if (manager->phase == PluginManagerPhase::Running) {
                auto committed = prepared.value().commit();
                if (committed.ok()) {
                    work.library = prepared.value().library();
                    work.result = Status{};
                    work.done = true;
                    work.stage = WorkStage::Complete;
                    --manager->active_loads;
                    manager->changed.notify_all();
                    return;
                }
                failure = std::move(committed).status();
            } else {
                failure =
                    Status(StatusCode::InvalidState, "Manager stopped before Library publish");
            }
        }
        // Transient dlclose/rollback must finish before actual-work accounting.
        { auto rollback = std::move(prepared).value(); }
    } else {
        failure = std::move(prepared).status();
    }
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        --manager->active_loads;
        --manager->loaded_count;
        work.result = std::move(failure);
        work.done = true;
        work.stage = WorkStage::Complete;
    }
    manager->changed.notify_all();
}

}  // namespace rodaq::plugin
