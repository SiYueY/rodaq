#include <rodaq/plugin/manager.hpp>

#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>

#include "lifecycle.hpp"

#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace rodaq::plugin {
namespace {

constexpr std::size_t kMaxWaitingLoads = 1024;

Result<PluginInstanceId> reserve_instance_id() {
    static std::atomic<PluginInstanceId> next{1};
    auto current = next.load();
    for (;;) {
        if (current == std::numeric_limits<PluginInstanceId>::max()) {
            return Status(StatusCode::ResourceExhausted, "Plugin Instance identity exhausted");
        }
        if (next.compare_exchange_weak(current, current + 1)) {
            return current;
        }
    }
}

struct LoadObserver {
    std::shared_ptr<ManagerState> manager;

    ~LoadObserver() {
        std::lock_guard<std::mutex> lock(manager->mutex);
        --manager->waiting_loads;
    }
};

Result<std::shared_ptr<LibraryState>> load_library(
    const std::shared_ptr<ManagerState>& manager, std::size_t index) {
    std::shared_ptr<LifecycleWork> work;
    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        if (manager->phase != PluginManagerPhase::Running) {
            return Status(StatusCode::InvalidState, "PluginManager is not Running");
        }
        const auto& selected = manager->catalog[index];
        for (std::size_t i = 0; i < index; ++i) {
            const auto& candidate = manager->catalog[i];
            if (candidate.library_path == selected.library_path &&
                candidate.info.plugin_id == selected.info.plugin_id &&
                candidate.info.plugin_name == selected.info.plugin_name &&
                candidate.info.plugin_version == selected.info.plugin_version) {
                index = i;
                break;
            }
        }
        work = manager->loads[index];
        if (work && work->done && work->result.ok()) {
            return work->library;
        }
        if (manager->waiting_loads == kMaxWaitingLoads) {
            return Status(StatusCode::ResourceExhausted, "Plugin load observer capacity");
        }
        if (!work || work->done) {
            if (manager->loaded_count == manager->options.max_loaded_plugins) {
                return Status(StatusCode::ResourceExhausted, "Loaded Plugin capacity");
            }
            work.reset();
            for (const auto& available : manager->load_pool) {
                if (available.use_count() == 1) {
                    work = available;
                    break;
                }
            }
            if (!work) {
                return Status(StatusCode::ResourceExhausted, "Plugin load record capacity");
            }
            work->catalog_index = index;
            work->done = false;
            work->submitted = false;
            work->reported = false;
            work->result = Status{};
            work->library.reset();
            if (!enqueue_locked(*manager, *work)) {
                return Status(StatusCode::ResourceExhausted, "Plugin lifecycle queue capacity");
            }
            manager->loads[index] = work;
            ++manager->loaded_count;
            ++manager->active_loads;
        }
        ++manager->waiting_loads;
    }
    LoadObserver observer{manager};
    manager->changed.notify_all();
    std::unique_lock<std::mutex> lock(manager->mutex);
    manager->changed.wait(
        lock, [&] { return work->done || manager->phase != PluginManagerPhase::Running; });
    if (work->done) {
        if (!work->result.ok()) {
            return work->result;
        }
        return work->library;
    }
    return manager->restart_required
               ? manager->failure
               : Status(StatusCode::InvalidState, "Manager stopped during load");
}

void abandon_creation(const std::shared_ptr<ManagerState>& state) noexcept {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->discovery.submitted) {
            state->discovery.done = true;
        }
    }
    request_shutdown(state);
}

}  // namespace

PluginManager::PluginManager(std::shared_ptr<ManagerState> state) noexcept
: state_(std::move(state)) {}

PluginManager::PluginManager(PluginManager&&) noexcept = default;

PluginManager& PluginManager::operator=(PluginManager&& other) noexcept {
    if (this != &other) {
        if (state_) {
            request_shutdown(state_);
        }
        state_ = std::move(other.state_);
    }
    return *this;
}

PluginManager::~PluginManager() noexcept {
    if (state_) {
        bool abandoned = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            abandoned = !state_->shutdown_requested;
        }
        if (abandoned) {
            enqueue_diagnostic(
                state_,
                "PluginManager abandoned without explicit shutdown; asynchronous drain requested");
        }
        request_shutdown(state_);
    }
}

Result<PluginManager> PluginManager::create(PluginManagerOptions options) {
    if (options.max_config_size > UINT32_MAX || options.max_instances == 0 ||
        options.max_loaded_plugins == 0 || options.shutdown_timeout.count() < 0) {
        return Status(StatusCode::InvalidArgument, "Invalid PluginManager options");
    }
    if (options.max_instances > 1024 || options.max_loaded_plugins > 256 ||
        options.search_paths.size() > kMaxPluginRoots) {
        return Status(StatusCode::ResourceExhausted, "PluginManager qualification resource limit");
    }
    auto token = []() -> Result<std::uint64_t> {
        try {
            return reserve_authority();
        } catch (const std::bad_alloc&) {
            return Status(StatusCode::ResourceExhausted, {});
        } catch (...) {
            return Status(StatusCode::Internal, {});
        }
    }();
    if (!token.ok()) {
        return token.status();
    }
    std::shared_ptr<ManagerState> state;
    try {
        state = std::make_shared<ManagerState>(std::move(options), token.value());
        state->self_retention = state;
        auto started = start_workers(state);
        if (!started.ok()) {
            return std::move(started).status();
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!enqueue_locked(*state, state->discovery)) {
                state->discovery.done = true;
                state->discovery.result = Status(StatusCode::ResourceExhausted, {});
            }
        }
        state->changed.notify_all();
        Status failure;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->changed.wait(
                lock, [&] { return state->discovery.done || state->restart_required; });
            if (state->discovery.done && state->discovery.result.ok() &&
                state->phase == PluginManagerPhase::Running) {
                return PluginManager(std::move(state));
            }
            failure = state->discovery.done && !state->discovery.result.ok()
                          ? state->discovery.result
                          : state->failure;
        }
        request_shutdown(state);
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (!state->restart_required) {
                state->changed.wait(
                    lock, [&] { return state->phase == PluginManagerPhase::Stopped; });
            }
        }
        return failure;
    } catch (const std::bad_alloc&) {
        if (state) {
            abandon_creation(state);
        } else {
            release_authority(token.value());
        }
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        if (state) {
            abandon_creation(state);
        } else {
            release_authority(token.value());
        }
        return Status(StatusCode::Internal, {});
    }
}

Result<std::vector<PluginInfo>> PluginManager::plugins() const {
    assert(state_);
    try {
        std::vector<PluginInfo> result;
        result.reserve(state_->catalog.size());
        for (const auto& entry : state_->catalog) {
            result.push_back(entry.info);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<PluginLibrary> PluginManager::load(std::string_view plugin_id) {
    assert(state_);
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->phase != PluginManagerPhase::Running) {
            return Status(StatusCode::InvalidState, "PluginManager is not Running");
        }
    }
    auto selected = select_plugin(state_->catalog, plugin_id, {});
    if (!selected.ok()) {
        return selected.status();
    }
    auto library = load_library(state_, selected.value());
    if (!library.ok()) {
        return library.status();
    }
    return PluginLibrary(state_, std::move(library).value());
}

Result<PluginLibrary> PluginManager::load(
    std::string_view plugin_id, std::string_view plugin_version) {
    assert(state_);
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->phase != PluginManagerPhase::Running) {
            return Status(StatusCode::InvalidState, "PluginManager is not Running");
        }
    }
    auto selected = select_plugin(state_->catalog, plugin_id, plugin_version);
    if (!selected.ok()) {
        return selected.status();
    }
    auto library = load_library(state_, selected.value());
    if (!library.ok()) {
        return library.status();
    }
    return PluginLibrary(state_, std::move(library).value());
}

PluginManagerPhase PluginManager::phase() const noexcept {
    assert(state_);
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->phase;
}

bool PluginManager::restart_required() const noexcept {
    assert(state_);
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->restart_required;
}

Result<void> PluginManager::shutdown(Deadline deadline) {
    assert(state_);
    if (current_manager_invocation(*state_)) {
        return Status(StatusCode::InvalidState, "Shutdown would await current invocation");
    }
    request_shutdown(state_);
    std::unique_lock<std::mutex> lock(state_->mutex);
    while (state_->phase != PluginManagerPhase::Stopped &&
           state_->phase != PluginManagerPhase::Degraded) {
        if (deadline.is_infinite()) {
            state_->changed.wait(lock);
        } else {
            const auto now = monotonic_now();
            if (now < 0 || now >= deadline.native().monotonic_deadline) {
                return Status(StatusCode::DeadlineExceeded, "Manager shutdown observer timed out");
            }
            state_->changed.wait_for(
                lock, std::chrono::nanoseconds(deadline.native().monotonic_deadline - now));
        }
    }
    if (state_->phase != PluginManagerPhase::Stopped) {
        return state_->failure;
    }
    lock.unlock();
    // Native worker/TLS cleanup was joined by the independently supervised reaper.
    std::lock_guard<std::mutex> completion(state_->completion_mutex);
    if (state_->monitor.joinable()) {
        state_->monitor.join();
    }
    return {};
}

PluginLibrary::PluginLibrary(
    std::shared_ptr<ManagerState> manager, std::shared_ptr<LibraryState> state) noexcept
: manager_(std::move(manager)), state_(std::move(state)) {}

PluginLibrary::PluginLibrary(PluginLibrary&&) noexcept = default;

PluginLibrary& PluginLibrary::operator=(PluginLibrary&&) noexcept = default;

PluginLibrary::~PluginLibrary() noexcept = default;

PluginLibrary::operator bool() const noexcept { return manager_ && state_; }

const PluginInfo& PluginLibrary::info() const noexcept {
    assert(state_);
    return state_->info;
}

Result<PluginInstance> PluginLibrary::create_instance(std::string_view config) const {
    assert(manager_ && state_);
    if (config.size() > manager_->options.max_config_size) {
        return Status(StatusCode::ResourceExhausted, "Plugin configuration size limit");
    }
    if ((!config.empty() && config.data() == nullptr) ||
        abi::utf8_prefix(config, config.size()) != config.size()) {
        return Status(StatusCode::InvalidArgument, "Plugin configuration is not UTF-8");
    }
    std::size_t slot = 0;
    {
        std::lock_guard<std::mutex> lock(manager_->mutex);
        if (manager_->phase != PluginManagerPhase::Running || state_->faulted) {
            return Status(StatusCode::InvalidState, "Plugin authority is not available");
        }
        while (slot < manager_->reserved_instances.size() && manager_->reserved_instances[slot]) {
            ++slot;
        }
        if (slot == manager_->reserved_instances.size()) {
            return Status(StatusCode::ResourceExhausted, "Plugin Instance capacity");
        }
        manager_->reserved_instances[slot] = true;
        manager_->instance_reserved_at[slot] = std::chrono::steady_clock::now();
        ++manager_->instance_count;
        ++manager_->active_creates;
    }
    std::shared_ptr<InstanceState> instance;
    try {
        const auto identity = reserve_instance_id();
        if (!identity.ok()) {
            throw std::bad_alloc();
        }
        instance = std::make_shared<InstanceState>(
            manager_, state_, config.empty() ? std::string{} : std::string(config), slot,
            identity.value());
        instance->initialization.instance = instance;
    } catch (...) {
        std::lock_guard<std::mutex> lock(manager_->mutex);
        manager_->reserved_instances[slot] = false;
        --manager_->instance_count;
        --manager_->active_creates;
        manager_->changed.notify_all();
        return Status(StatusCode::ResourceExhausted, {});
    }
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(manager_->mutex);
        manager_->instances[slot] = instance;
        queued = enqueue_locked(*manager_, instance->initialization);
    }
    if (!queued) {
        fail_initialization(manager_, instance, Status(StatusCode::ResourceExhausted, {}), false);
    }
    manager_->changed.notify_all();
    std::unique_lock<std::mutex> lock(manager_->mutex);
    manager_->changed.wait(lock, [&] {
        return instance->initialization.done || manager_->phase != PluginManagerPhase::Running;
    });
    if (instance->initialization.done) {
        if (!instance->initialization.result.ok()) {
            return instance->initialization.result;
        }
        return PluginInstance(instance);
    }
    return manager_->restart_required
               ? manager_->failure
               : Status(StatusCode::InvalidState, "Manager stopped during initialization");
}

}  // namespace rodaq::plugin
