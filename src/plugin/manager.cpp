#include <rodaq/plugin/manager.hpp>

#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>

#include "lifecycle.hpp"

#include <algorithm>
#include <atomic>
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

// Direct single-flight loader: bounded catalog -> dlopen -> dlsym -> ABI &
// descriptor check -> resident publish. No 30s watchdog on trusted local
// deployment bootstrap.
Result<std::shared_ptr<LibraryState>> load_library(
    const std::shared_ptr<ManagerState>& manager, std::size_t index) {
    bool is_leader = false;
    {
        std::unique_lock<std::mutex> lock(manager->mutex);
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
        auto& slot = manager->load_slots[index];
        for (;;) {
            if (manager->phase != PluginManagerPhase::Running) {
                return Status(StatusCode::InvalidState, "PluginManager is not Running");
            }
            if (slot.loaded && slot.library) {
                return slot.library;
            }
            if (!slot.in_flight) {
                if (manager->loaded_count == manager->options.max_loaded_plugins) {
                    return Status(StatusCode::ResourceExhausted, "Loaded Plugin capacity");
                }
                slot.in_flight = true;
                slot.last_result = Status{};
                ++manager->loaded_count;
                ++manager->active_loads;
                is_leader = true;
                break;
            }
            manager->changed.wait(lock, [&] {
                return !slot.in_flight || manager->phase != PluginManagerPhase::Running;
            });
            if (slot.loaded && slot.library) {
                return slot.library;
            }
            if (!slot.in_flight && !slot.last_result.ok()) {
                return slot.last_result;
            }
        }
    }

    assert(is_leader);
    (void)is_leader;

    Status failure;
    std::shared_ptr<LibraryState> committed_lib;
    bool abi_exception = false;
    try {
        auto prepared = prepare_library(manager->catalog[index]);
        if (prepared.ok()) {
            {
                std::lock_guard<std::mutex> lock(manager->mutex);
                if (manager->phase == PluginManagerPhase::Running) {
                    auto committed = prepared.value().commit();
                    if (committed.ok()) {
                        committed_lib = prepared.value().library();
                    } else {
                        failure = std::move(committed).status();
                    }
                } else {
                    failure =
                        Status(StatusCode::InvalidState, "Manager stopped before Library publish");
                }
            }
            if (!committed_lib) {
                auto rollback = std::move(prepared).value();
            }
        } else {
            failure = std::move(prepared).status();
        }
    } catch (const std::bad_alloc&) {
        failure = Status(StatusCode::ResourceExhausted, "Out of memory loading Plugin");
    } catch (...) {
        abi_exception = true;
        failure = Status(StatusCode::Internal, "Exception crossed load ABI; restart required");
    }

    {
        std::lock_guard<std::mutex> lock(manager->mutex);
        auto& slot = manager->load_slots[index];
        slot.in_flight = false;
        --manager->active_loads;
        if (committed_lib) {
            slot.loaded = true;
            slot.library = committed_lib;
            slot.last_result = Status{};
        } else {
            --manager->loaded_count;
            slot.last_result = failure;
            if (abi_exception) {
                degrade_locked(*manager, failure);
            }
        }
    }
    manager->changed.notify_all();
    if (abi_exception) {
        enqueue_diagnostic(
            manager, "Exception crossed load ABI; restart required", &manager->catalog[index].info,
            0, "native-load");
    }
    if (committed_lib) {
        return committed_lib;
    }
    return failure;
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

    // Synchronous bounded catalog scan (trusted local deployment bootstrap).
    auto catalog = discover_plugins(options.search_paths);
    if (!catalog.ok()) {
        release_authority(token.value());
        return catalog.status();
    }

    std::shared_ptr<ManagerState> state;
    try {
        state = std::make_shared<ManagerState>(
            std::move(options), std::move(catalog).value(), token.value());
        state->self_retention = state;
        auto started = start_workers(state);
        if (!started.ok()) {
            return std::move(started).status();
        }
        return PluginManager(std::move(state));
    } catch (const std::bad_alloc&) {
        if (state) {
            request_shutdown(state);
        } else {
            release_authority(token.value());
        }
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        if (state) {
            request_shutdown(state);
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
