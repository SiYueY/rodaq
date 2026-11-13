#include "plugin_state.hpp"

#include "abi_support.hpp"
#include "loader.hpp"
#include "semver.hpp"
#include "testing_access.hpp"

#include <rodaq/plugin/manager.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace rodaq::plugin {
namespace {

std::mutex authority_mutex;
std::shared_ptr<PluginManagerState> authority_state;

std::int64_t monotonic_now_nanoseconds() noexcept {
    return std::chrono::duration_cast<DeadlineDuration>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string load_key(const CatalogEntry& entry) {
    return entry.artifact_path.generic_string() + "\n" + entry.manifest.plugin_id + "\n" +
           entry.manifest.plugin_version;
}

bool abi_compatible(const Manifest& manifest) noexcept {
    return manifest.abi_major == kSupportedPluginAbiMajor &&
           manifest.abi_minor <= kSupportedPluginAbiMinor;
}

Result<CatalogEntry> resolve_entry(
    const CatalogGeneration& catalog, std::string_view plugin_id, std::string_view plugin_version,
    bool exact_version) {
    std::vector<const CatalogEntry*> candidates;
    for (const auto& entry : catalog.entries) {
        if (entry.manifest.plugin_id == plugin_id && abi_compatible(entry.manifest)) {
            if (exact_version && entry.manifest.plugin_version != plugin_version) {
                continue;
            }
            candidates.push_back(&entry);
        }
    }
    if (candidates.empty()) {
        return Status{StatusCode::NotFound, "plugin was not found"};
    }

    if (exact_version) {
        const auto* selected = candidates.front();
        for (const auto* candidate : candidates) {
            if (candidate->manifest.plugin_version == selected->manifest.plugin_version &&
                candidate->artifact.identity.device != selected->artifact.identity.device) {
                return Status{StatusCode::Conflict, "plugin version maps to multiple artifacts"};
            }
            if (candidate->artifact.identity.inode != selected->artifact.identity.inode) {
                return Status{StatusCode::Conflict, "plugin version maps to multiple artifacts"};
            }
        }
        return *selected;
    }

    const CatalogEntry* best = nullptr;
    for (const auto* candidate : candidates) {
        auto parsed = parse_semver(candidate->manifest.plugin_version);
        if (!parsed.ok() || !parsed.value().stable()) {
            continue;
        }
        if (best == nullptr) {
            best = candidate;
            continue;
        }
        auto best_version = parse_semver(best->manifest.plugin_version);
        if (!best_version.ok()) {
            best = candidate;
            continue;
        }
        if (compare_semver_precedence(parsed.value(), best_version.value()) > 0) {
            best = candidate;
        }
    }
    if (best == nullptr) {
        return Status{StatusCode::Unavailable, "no compatible stable plugin release"};
    }

    auto best_version = parse_semver(best->manifest.plugin_version);
    if (!best_version.ok()) {
        return best_version.status();
    }
    for (const auto* candidate : candidates) {
        auto parsed = parse_semver(candidate->manifest.plugin_version);
        if (!parsed.ok() || !parsed.value().stable() ||
            compare_semver_precedence(parsed.value(), best_version.value()) != 0) {
            continue;
        }
        if (parsed.value().identity != best_version.value().identity) {
            return Status{StatusCode::Conflict, "highest plugin precedence is ambiguous"};
        }
        if (candidate->artifact.identity.device != best->artifact.identity.device ||
            candidate->artifact.identity.inode != best->artifact.identity.inode) {
            return Status{StatusCode::Conflict, "plugin identity maps to multiple artifacts"};
        }
    }
    return *best;
}

bool valid_foundation_instance(rodaq_plugin_instance_t instance) noexcept {
    return instance.context != nullptr && instance.vtable != nullptr &&
           instance.vtable->header.struct_size >= sizeof(rodaq_plugin_instance_vtable_t) &&
           instance.vtable->configure != nullptr && instance.vtable->query != nullptr &&
           instance.vtable->shutdown != nullptr && instance.vtable->destroy != nullptr;
}

bool default_invalid_instance(rodaq_plugin_instance_t instance) noexcept {
    return instance.context == nullptr && instance.vtable == nullptr;
}

}  // namespace

PluginManagerState::PluginManagerState(
    PluginManagerOptions options, LifecyclePolicy lifecycle_policy)
: options_(std::move(options)), lifecycle_policy_(lifecycle_policy) {}

std::shared_ptr<LifecycleWorkState> PluginManagerState::begin_lifecycle_work(
    LifecycleWorkKind kind) {
    std::lock_guard<std::mutex> lock{mutex_};
    const auto id = next_lifecycle_work_id_++;
    auto work = std::make_shared<LifecycleWorkState>(id, kind);
    lifecycle_work_.emplace(id, work);
    return work;
}

void PluginManagerState::finish_lifecycle_work(const std::shared_ptr<LifecycleWorkState>& work) {
    if (work == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock{mutex_};
        lifecycle_work_.erase(work->id());
    }
    condition_.notify_all();
}

void PluginManagerState::start_workers() {
    auto state = shared_from_this();
    try {
        constexpr std::size_t kCloseWorkerCount = 2u;
        close_workers_.reserve(kCloseWorkerCount);
        for (std::size_t index = 0u; index < kCloseWorkerCount; ++index) {
            close_workers_.emplace_back([state] { state->close_worker_loop(); });
        }
        supervision_worker_ = std::thread([state] { state->supervision_loop(); });
    } catch (...) {
        stop_workers_.store(true, std::memory_order_release);
        condition_.notify_all();
        for (auto& worker : close_workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

PluginManagerPhase PluginManagerState::phase() const noexcept {
    return phase_.load(std::memory_order_acquire);
}

bool PluginManagerState::restart_required() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return restart_required_;
}

void PluginManagerState::signal_lock() noexcept { condition_.notify_all(); }

void PluginManagerState::add_diagnostic(
    std::string plugin_id, std::string plugin_version, PluginInstanceId instance_id,
    StatusCode code, std::string reason, std::string message) {
    if (message.size() > 512u) {
        message.resize(512u);
    }
    if (diagnostics_.size() >= 64u) {
        diagnostics_.pop_front();
    }
    diagnostics_.push_back(DiagnosticRecord{
        std::move(plugin_id), std::move(plugin_version), instance_id, code, std::move(reason),
        std::move(message), monotonic_now_nanoseconds()});
}

void PluginManagerState::mark_degraded(StatusCode code, std::string_view message) noexcept {
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (phase_.load(std::memory_order_acquire) == PluginManagerPhase::Degraded) {
            return;
        }
        phase_.store(PluginManagerPhase::Degraded, std::memory_order_release);
        restart_required_ = true;
        try {
            add_diagnostic({}, {}, 0u, code, "plugin_manager_degraded", std::string{message});
        } catch (...) {
            // Degraded state publication must survive diagnostic allocation failure.
        }
    }
    condition_.notify_all();
}

void PluginManagerState::mark_library_faulted(
    const std::shared_ptr<PluginLibraryState>& library) noexcept {
    if (library != nullptr) {
        library->mark_faulted();
    }
    mark_degraded(StatusCode::Internal, "plugin library is faulted");
}

Result<void> PluginManagerState::refresh() {
    std::shared_future<Result<std::shared_ptr<CatalogGeneration>>> operation;
    bool creator = false;
    std::shared_ptr<std::promise<Result<std::shared_ptr<CatalogGeneration>>>> promise;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (phase_.load(std::memory_order_acquire) != PluginManagerPhase::Running) {
            return Status{StatusCode::InvalidState, "plugin manager is not running"};
        }
        if (refresh_active_) {
            operation = refresh_operation_;
        } else {
            promise = std::make_shared<std::promise<Result<std::shared_ptr<CatalogGeneration>>>>();
            operation = promise->get_future().share();
            refresh_operation_ = operation;
            refresh_active_ = true;
            active_refresh_count_ = 1u;
            creator = true;
        }
    }

    if (creator) {
        std::shared_ptr<LifecycleWorkState> work;
        try {
            work = begin_lifecycle_work(LifecycleWorkKind::Refresh);
        } catch (...) {
            std::lock_guard<std::mutex> lock{mutex_};
            refresh_active_ = false;
            active_refresh_count_ = 0u;
            return Status{StatusCode::ResourceExhausted, "catalog refresh bookkeeping failed"};
        }
        auto state = shared_from_this();
        const bool submitted = lifecycle_executor_.submit([state, promise, work] {
            work->set_stage(LifecycleStage::CatalogScan);
            auto discovered = discover_catalog(state->options_.search_paths);
            work->set_stage(LifecycleStage::CatalogBuild);
            Result<std::shared_ptr<CatalogGeneration>> result =
                discovered.ok() ? Result<std::shared_ptr<CatalogGeneration>>{std::make_shared<
                                      CatalogGeneration>(std::move(discovered).value())}
                                : Result<std::shared_ptr<CatalogGeneration>>{discovered.status()};
            {
                std::lock_guard<std::mutex> lock{state->mutex_};
                if (result.ok() &&
                    state->phase_.load(std::memory_order_acquire) != PluginManagerPhase::Running) {
                    result = Status{
                        StatusCode::InvalidState, "plugin manager stopped during catalog refresh"};
                }
                if (result.ok()) {
                    state->catalog_ = result.value();
                }
                state->refresh_active_ = false;
                state->active_refresh_count_ = 0u;
            }
            state->finish_lifecycle_work(work);
            state->condition_.notify_all();
            promise->set_value(std::move(result));
        });
        if (!submitted) {
            std::lock_guard<std::mutex> lock{mutex_};
            refresh_active_ = false;
            active_refresh_count_ = 0u;
            lifecycle_work_.erase(work->id());
            return Status{StatusCode::ResourceExhausted, "lifecycle executor is saturated"};
        }
    }

    if (operation.wait_for(lifecycle_policy_.catalog_stage) != std::future_status::ready) {
        mark_degraded(StatusCode::DeadlineExceeded, "catalog refresh timed out");
        return Status{StatusCode::DeadlineExceeded, "catalog refresh timed out"};
    }
    try {
        const auto result = operation.get();
        return result.ok() ? Result<void>{} : Result<void>{result.status()};
    } catch (...) {
        mark_degraded(StatusCode::Internal, "catalog refresh future failed");
        return Status{StatusCode::Internal, "catalog refresh future failed"};
    }
}

Result<std::vector<PluginInfo>> PluginManagerState::plugins() const {
    std::lock_guard<std::mutex> lock{mutex_};
    std::vector<PluginInfo> result;
    if (catalog_ != nullptr) {
        result.reserve(catalog_->entries.size());
        for (const auto& entry : catalog_->entries) {
            result.push_back(PluginInfo{
                entry.manifest.plugin_id, entry.manifest.plugin_name,
                entry.manifest.plugin_version});
        }
    }
    return result;
}

Result<std::shared_ptr<PluginLibraryState>> PluginManagerState::load(
    std::string_view plugin_id, std::string_view plugin_version, bool exact_version) {
    CatalogEntry entry;
    std::string key;
    std::shared_future<Result<std::shared_ptr<PluginLibraryState>>> operation;
    bool creator = false;
    std::shared_ptr<std::promise<Result<std::shared_ptr<PluginLibraryState>>>> promise;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (phase_.load(std::memory_order_acquire) != PluginManagerPhase::Running) {
            return Status{StatusCode::InvalidState, "plugin manager is not running"};
        }
        if (catalog_ == nullptr) {
            return Status{StatusCode::NotFound, "plugin catalog has not been refreshed"};
        }
        auto resolved = resolve_entry(*catalog_, plugin_id, plugin_version, exact_version);
        if (!resolved.ok()) {
            return resolved.status();
        }
        entry = std::move(resolved).value();
        key = load_key(entry);

        const auto loaded = loaded_plugins_.find(key);
        if (loaded != loaded_plugins_.end()) {
            return loaded->second;
        }
        const auto pending = load_operations_.find(key);
        if (pending != load_operations_.end()) {
            operation = pending->second;
        } else {
            if (loaded_plugins_.size() + load_operations_.size() >= options_.max_loaded_plugins) {
                return Status{StatusCode::ResourceExhausted, "loaded plugin limit reached"};
            }
            promise = std::make_shared<std::promise<Result<std::shared_ptr<PluginLibraryState>>>>();
            operation = promise->get_future().share();
            load_operations_.emplace(key, operation);
            ++active_load_count_;
            creator = true;
        }
    }

    if (creator) {
        std::shared_ptr<LifecycleWorkState> work;
        try {
            work = begin_lifecycle_work(LifecycleWorkKind::Load);
        } catch (...) {
            std::lock_guard<std::mutex> lock{mutex_};
            load_operations_.erase(key);
            if (active_load_count_ > 0u) {
                --active_load_count_;
            }
            return Status{StatusCode::ResourceExhausted, "plugin load bookkeeping failed"};
        }
        auto state = shared_from_this();
        const bool submitted = lifecycle_executor_.submit([state, entry, key, promise, work] {
            work->set_stage(LifecycleStage::NativeLoad);
            auto loaded = load_plugin_library(entry);
            {
                std::lock_guard<std::mutex> lock{state->mutex_};
                if (loaded.ok() &&
                    state->phase_.load(std::memory_order_acquire) != PluginManagerPhase::Running) {
                    loaded = Status{StatusCode::InvalidState, "plugin manager stopped during load"};
                }
                if (loaded.ok()) {
                    state->loaded_plugins_[key] = loaded.value();
                }
                state->load_operations_.erase(key);
                if (state->active_load_count_ > 0u) {
                    --state->active_load_count_;
                }
            }
            state->finish_lifecycle_work(work);
            state->condition_.notify_all();
            promise->set_value(std::move(loaded));
        });
        if (!submitted) {
            std::lock_guard<std::mutex> lock{mutex_};
            load_operations_.erase(key);
            if (active_load_count_ > 0u) {
                --active_load_count_;
            }
            lifecycle_work_.erase(work->id());
            return Status{StatusCode::ResourceExhausted, "lifecycle executor is saturated"};
        }
    }

    if (operation.wait_for(lifecycle_policy_.load_stage) != std::future_status::ready) {
        mark_degraded(StatusCode::DeadlineExceeded, "plugin load timed out");
        return Status{StatusCode::DeadlineExceeded, "plugin load timed out"};
    }
    try {
        return operation.get();
    } catch (...) {
        mark_degraded(StatusCode::Internal, "plugin load future failed");
        return Status{StatusCode::Internal, "plugin load future failed"};
    }
}

Result<std::shared_ptr<PluginInstanceState>> PluginManagerState::create_instance(
    const std::shared_ptr<PluginLibraryState>& library,
    const std::shared_ptr<const ServiceRegistryState>& services, std::string_view config) {
    if (library == nullptr || services == nullptr) {
        return Status{StatusCode::InvalidArgument, "plugin library or services are missing"};
    }
    if (config.size() > options_.max_config_size) {
        return Status{StatusCode::ResourceExhausted, "plugin config exceeds size limit"};
    }
    if (!valid_utf8_text(config)) {
        return Status{StatusCode::InvalidArgument, "plugin config is not valid UTF-8"};
    }

    std::shared_ptr<PluginInstanceState> instance;
    std::shared_ptr<std::promise<Result<std::shared_ptr<PluginInstanceState>>>> promise;
    std::shared_future<Result<std::shared_ptr<PluginInstanceState>>> future;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (phase_.load(std::memory_order_acquire) != PluginManagerPhase::Running) {
            return Status{StatusCode::InvalidState, "plugin manager is not running"};
        }
        if (library->phase() != PluginLibraryPhase::Loaded) {
            return Status{StatusCode::InvalidState, "plugin library is faulted"};
        }
        if (instances_.size() >= options_.max_instances) {
            return Status{StatusCode::ResourceExhausted, "plugin instance limit reached"};
        }

        const auto id = next_instance_id_++;
        instance = std::make_shared<PluginInstanceState>(id, library, services, weak_from_this());
        instances_.emplace(id, instance);
        ++active_create_count_;
        promise = std::make_shared<std::promise<Result<std::shared_ptr<PluginInstanceState>>>>();
        future = promise->get_future().share();
    }

    auto manager = shared_from_this();
    std::shared_ptr<LifecycleWorkState> work;
    try {
        work = begin_lifecycle_work(LifecycleWorkKind::Create);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (active_create_count_ > 0u) {
                --active_create_count_;
            }
        }
        manager->finish_closed_instance(instance->id());
        instance->mark_initialization_closed();
        return Status{StatusCode::ResourceExhausted, "plugin create bookkeeping failed"};
    }
    const std::string config_copy{config};
    const bool submitted = lifecycle_executor_.submit([manager, instance, services, config_copy,
                                                       promise, work] {
        auto finish_create = [&manager] {
            {
                std::lock_guard<std::mutex> lock{manager->mutex_};
                if (manager->active_create_count_ > 0u) {
                    --manager->active_create_count_;
                }
            }
            manager->condition_.notify_all();
        };
        const auto service_reference = make_service_registry_reference(services);
        work->set_stage(LifecycleStage::NativeCreate);
        rodaq_plugin_instance_t raw{};
        std::array<std::uint8_t, interface_support::kErrorMessageCapacity> create_error_storage{};
        rodaq_error_t create_error{};
        create_error.message.data = create_error_storage.data();
        create_error.message.capacity = static_cast<std::uint32_t>(create_error_storage.size());
        const auto create_code = instance->library()->entry()->create(&raw, &create_error);
        auto create_result = result_from_abi_call(create_code, &create_error);
        if (!is_known_status_code(create_code)) {
            manager->mark_library_faulted(instance->library());
            instance->mark_unresolved(
                Status{StatusCode::Internal, "plugin create returned unknown status"});
            manager->finish_lifecycle_work(work);
            finish_create();
            promise->set_value(
                Status{StatusCode::Internal, "plugin create returned unknown status"});
            return;
        }
        if (create_code != RODAQ_STATUS_OK) {
            auto failed = std::move(create_result).status();
            if (default_invalid_instance(raw)) {
                manager->finish_closed_instance(instance->id());
                instance->mark_initialization_closed();
                manager->finish_lifecycle_work(work);
                finish_create();
                promise->set_value(std::move(failed));
            } else {
                manager->mark_library_faulted(instance->library());
                instance->mark_unresolved(Status{
                    StatusCode::Internal, "plugin create produced non-default failure output"});
                manager->finish_lifecycle_work(work);
                finish_create();
                promise->set_value(Status{
                    StatusCode::Internal, "plugin create produced non-default failure output"});
            }
            return;
        }
        if (!create_result.ok()) {
            auto failed = std::move(create_result).status();
            manager->mark_library_faulted(instance->library());
            if (valid_foundation_instance(raw)) {
                instance->set_raw_instance(raw);
                (void)instance->request_close();
            } else {
                instance->mark_unresolved(std::move(failed));
            }
            manager->finish_lifecycle_work(work);
            finish_create();
            promise->set_value(
                Status{StatusCode::Internal, "plugin create returned an invalid success result"});
            return;
        }
        if (!valid_foundation_instance(raw)) {
            manager->mark_library_faulted(instance->library());
            instance->mark_unresolved(
                Status{StatusCode::Internal, "plugin create produced malformed instance"});
            manager->finish_lifecycle_work(work);
            finish_create();
            promise->set_value(
                Status{StatusCode::Internal, "plugin create produced malformed instance"});
            return;
        }

        instance->set_raw_instance(raw);
        work->set_stage(LifecycleStage::FoundationValidation);
        work->set_stage(LifecycleStage::NativeConfigure);
        rodaq_config_t abi_config{};
        abi_config.header.struct_size = sizeof(abi_config);
        abi_config.json = string_to_abi(config_copy);
        std::array<std::uint8_t, interface_support::kErrorMessageCapacity>
            configure_error_storage{};
        rodaq_error_t configure_error{};
        configure_error.message.data = configure_error_storage.data();
        configure_error.message.capacity =
            static_cast<std::uint32_t>(configure_error_storage.size());
        const auto configure_code =
            raw.vtable->configure(raw.context, &service_reference, &abi_config, &configure_error);
        auto configure_result = result_from_abi_call(configure_code, &configure_error);
        if (!is_known_status_code(configure_code)) {
            manager->mark_library_faulted(instance->library());
            (void)instance->request_close();
            manager->finish_lifecycle_work(work);
            finish_create();
            promise->set_value(
                Status{StatusCode::Internal, "plugin configure returned unknown status"});
            return;
        }
        if (configure_code != RODAQ_STATUS_OK) {
            auto status = std::move(configure_result).status();
            (void)instance->request_close();
            manager->finish_lifecycle_work(work);
            finish_create();
            promise->set_value(status);
            return;
        }
        if (!configure_result.ok()) {
            manager->mark_library_faulted(instance->library());
            (void)instance->request_close();
            manager->finish_lifecycle_work(work);
            finish_create();
            promise->set_value(Status{
                StatusCode::Internal, "plugin configure returned an invalid success result"});
            return;
        }

        bool published = false;
        work->set_stage(LifecycleStage::OpenCommit);
        {
            std::lock_guard<std::mutex> lock{manager->mutex_};
            if (manager->phase_.load(std::memory_order_acquire) == PluginManagerPhase::Running) {
                published = instance->open_for_use();
            }
            if (manager->active_create_count_ > 0u) {
                --manager->active_create_count_;
            }
        }
        manager->finish_lifecycle_work(work);
        manager->condition_.notify_all();
        if (!published) {
            (void)instance->request_close();
            promise->set_value(
                Status{StatusCode::InvalidState, "plugin manager stopped during create"});
            return;
        }
        promise->set_value(instance);
    });

    if (!submitted) {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (active_create_count_ > 0u) {
                --active_create_count_;
            }
        }
        manager->finish_lifecycle_work(work);
        manager->finish_closed_instance(instance->id());
        instance->mark_initialization_closed();
        condition_.notify_all();
        return Status{StatusCode::ResourceExhausted, "lifecycle executor is saturated"};
    }

    if (future.wait_for(lifecycle_policy_.initialization_stage) != std::future_status::ready) {
        mark_degraded(StatusCode::DeadlineExceeded, "plugin create/configure timed out");
        return Status{StatusCode::DeadlineExceeded, "plugin create/configure timed out"};
    }
    try {
        return future.get();
    } catch (...) {
        mark_degraded(StatusCode::Internal, "plugin create future failed");
        return Status{StatusCode::Internal, "plugin create future failed"};
    }
}

void PluginManagerState::request_close(const std::shared_ptr<PluginInstanceState>&) noexcept {
    condition_.notify_all();
}

void PluginManagerState::finish_closed_instance(PluginInstanceId id) noexcept {
    {
        std::lock_guard<std::mutex> lock{mutex_};
        instances_.erase(id);
    }
    condition_.notify_all();
}

std::shared_ptr<PluginInstanceState> PluginManagerState::find_ready_to_close_locked() {
    for (const auto& [id, instance] : instances_) {
        (void)id;
        if (instance->phase() == PluginInstancePhase::Closing && instance->try_begin_close()) {
            return instance;
        }
    }
    return nullptr;
}

bool PluginManagerState::has_closing_instance_locked() const {
    for (const auto& [id, instance] : instances_) {
        (void)id;
        if (instance->phase() == PluginInstancePhase::Closing) {
            return true;
        }
    }
    return false;
}

void PluginManagerState::close_worker_loop() {
    while (!stop_workers_.load(std::memory_order_acquire)) {
        std::shared_ptr<PluginInstanceState> instance;
        {
            std::unique_lock<std::mutex> lock{mutex_};
            condition_.wait(lock, [this] {
                return stop_workers_.load(std::memory_order_acquire) ||
                       has_closing_instance_locked();
            });
            if (stop_workers_.load(std::memory_order_acquire)) {
                return;
            }
            instance = find_ready_to_close_locked();
        }
        if (instance != nullptr) {
            instance->run_close(options_.shutdown_timeout, weak_from_this());
        }
    }
}

void PluginManagerState::supervision_loop() {
    while (!stop_workers_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock{mutex_};
            condition_.wait_for(lock, std::chrono::milliseconds{10}, [this] {
                return stop_workers_.load(std::memory_order_acquire);
            });
            if (stop_workers_.load(std::memory_order_acquire)) {
                return;
            }
        }
        bool should_degrade = false;
        std::vector<std::shared_ptr<LifecycleWorkState>> lifecycle_work;
        std::vector<std::shared_ptr<PluginInstanceState>> instances;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            lifecycle_work.reserve(lifecycle_work_.size());
            for (const auto& [id, work] : lifecycle_work_) {
                (void)id;
                lifecycle_work.push_back(work);
            }
            for (const auto& [id, instance] : instances_) {
                (void)id;
                instances.push_back(instance);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        for (const auto& work : lifecycle_work) {
            const auto stage = work->stage();
            const auto timeout = lifecycle_stage_timeout(work->kind(), stage, lifecycle_policy_);
            if (now - work->stage_started() > timeout) {
                should_degrade = true;
                break;
            }
        }
        for (const auto& instance : instances) {
            if (instance->phase() != PluginInstancePhase::Closing) {
                continue;
            }
            const auto started = instance->close_stage_started();
            const auto stage = instance->close_stage();
            const auto timeout = stage == CloseStage::NativeShutdown
                                     ? options_.shutdown_timeout
                                     : lifecycle_policy_.close_stage;
            if (started.time_since_epoch().count() != 0 && now - started > timeout) {
                should_degrade = true;
                break;
            }
        }
        if (should_degrade) {
            mark_degraded(StatusCode::DeadlineExceeded, "plugin close stage timed out");
        }
    }
}

bool PluginManagerState::success_predicate_locked() const noexcept {
    return !restart_required_ &&
           phase_.load(std::memory_order_acquire) != PluginManagerPhase::Degraded &&
           active_refresh_count_ == 0u && active_load_count_ == 0u && active_create_count_ == 0u &&
           !refresh_active_ && load_operations_.empty() && lifecycle_work_.empty() &&
           instances_.empty();
}

void PluginManagerState::begin_shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (phase_.load(std::memory_order_acquire) == PluginManagerPhase::Running) {
            phase_.store(PluginManagerPhase::ShuttingDown, std::memory_order_release);
        }
        for (const auto& [id, instance] : instances_) {
            (void)id;
            if (instance->phase() == PluginInstancePhase::Open) {
                (void)instance->request_close();
            }
        }
    }
    condition_.notify_all();
}

Result<void> PluginManagerState::shutdown(Deadline deadline) {
    begin_shutdown();
    bool perform_teardown = false;
    {
        std::unique_lock<std::mutex> lock{mutex_};
        auto predicate = [this] {
            return phase_.load(std::memory_order_acquire) == PluginManagerPhase::Degraded ||
                   success_predicate_locked() || teardown_started_;
        };
        if (deadline.is_infinite()) {
            condition_.wait(lock, predicate);
        } else {
            const auto time_point =
                std::chrono::steady_clock::time_point{DeadlineDuration{deadline.nanoseconds()}};
            if (!condition_.wait_until(lock, time_point, predicate)) {
                return Status{
                    StatusCode::DeadlineExceeded, "plugin manager shutdown wait timed out"};
            }
        }
        if (phase_.load(std::memory_order_acquire) == PluginManagerPhase::Degraded ||
            restart_required_) {
            return Status{StatusCode::Internal, "plugin manager shutdown is degraded"};
        }
        if (!teardown_complete_ && !teardown_started_) {
            teardown_started_ = true;
            perform_teardown = true;
        }
        if (teardown_complete_) {
            phase_.store(PluginManagerPhase::Stopped, std::memory_order_release);
            return {};
        }
    }

    if (perform_teardown) {
        stop_workers_.store(true, std::memory_order_release);
        condition_.notify_all();
        lifecycle_executor_.stop_and_join();
        for (auto& worker : close_workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        if (supervision_worker_.joinable()) {
            supervision_worker_.join();
        }
        {
            std::lock_guard<std::mutex> lock{mutex_};
            teardown_complete_ = true;
        }
        condition_.notify_all();
    }

    std::unique_lock<std::mutex> lock{mutex_};
    if (deadline.is_infinite()) {
        condition_.wait(lock, [this] { return teardown_complete_; });
    } else {
        const auto time_point =
            std::chrono::steady_clock::time_point{DeadlineDuration{deadline.nanoseconds()}};
        if (!condition_.wait_until(lock, time_point, [this] { return teardown_complete_; })) {
            return Status{StatusCode::DeadlineExceeded, "plugin manager teardown timed out"};
        }
    }
    phase_.store(PluginManagerPhase::Stopped, std::memory_order_release);
    return {};
}

Result<PluginDiagnostics> PluginManagerState::diagnostics() const {
    std::lock_guard<std::mutex> lock{mutex_};
    PluginDiagnostics diagnostics;
    diagnostics.phase = phase_.load(std::memory_order_acquire);
    diagnostics.restart_required = restart_required_;
    diagnostics.loaded_plugin_count = loaded_plugins_.size();
    diagnostics.recent_errors.assign(diagnostics_.begin(), diagnostics_.end());
    for (const auto& [id, instance] : instances_) {
        (void)id;
        const auto state = instance->phase();
        if (state == PluginInstancePhase::Open) {
            ++diagnostics.live_instance_count;
        } else if (state == PluginInstancePhase::Closing) {
            ++diagnostics.closing_instance_count;
        } else if (state == PluginInstancePhase::Unresolved) {
            ++diagnostics.unresolved_instance_count;
        }
    }
    for (const auto& [key, library] : loaded_plugins_) {
        (void)key;
        if (library->phase() == PluginLibraryPhase::Faulted) {
            ++diagnostics.faulted_plugin_count;
        }
    }
    return diagnostics;
}

PluginManager::PluginManager(std::shared_ptr<PluginManagerState> state) noexcept
: state_(std::move(state)) {}

Result<PluginManager> PluginManagerTestingAccess::create(
    PluginManagerOptions options, LifecyclePolicy lifecycle_policy) {
    if (options.max_instances == 0u || options.max_loaded_plugins == 0u ||
        options.max_config_size > UINT32_MAX || options.shutdown_timeout.count() <= 0) {
        return Status{StatusCode::InvalidArgument, "plugin manager options are invalid"};
    }
    std::lock_guard<std::mutex> lock{authority_mutex};
    if (authority_state != nullptr) {
        return Status{StatusCode::Conflict, "plugin manager authority already exists"};
    }
    auto state = std::make_shared<PluginManagerState>(std::move(options), lifecycle_policy);
    state->start_workers();
    authority_state = state;
    return PluginManager{std::move(state)};
}

Result<PluginManager> PluginManager::create(PluginManagerOptions options) {
    if (options.max_instances == 0u || options.max_loaded_plugins == 0u ||
        options.max_config_size > UINT32_MAX || options.shutdown_timeout.count() <= 0) {
        return Status{StatusCode::InvalidArgument, "plugin manager options are invalid"};
    }

    std::lock_guard<std::mutex> lock{authority_mutex};
    if (authority_state != nullptr) {
        return Status{StatusCode::Conflict, "plugin manager authority already exists"};
    }

    std::shared_ptr<PluginManagerState> state;
    try {
        state = std::make_shared<PluginManagerState>(std::move(options));
        state->start_workers();
    } catch (const std::bad_alloc&) {
        return Status{StatusCode::ResourceExhausted, "plugin manager allocation failed"};
    } catch (...) {
        return Status{StatusCode::Internal, "plugin manager worker startup failed"};
    }
    authority_state = state;
    return PluginManager{std::move(state)};
}

PluginManager::PluginManager(PluginManager&&) noexcept = default;

PluginManager& PluginManager::operator=(PluginManager&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (state_ != nullptr) {
        state_->begin_shutdown();
    }
    state_ = std::move(other.state_);
    return *this;
}

PluginManager::~PluginManager() noexcept {
    if (state_ != nullptr) {
        state_->begin_shutdown();
    }
}

PluginManager::operator bool() const noexcept { return state_ != nullptr; }

Result<void> PluginManager::refresh() {
    return state_ != nullptr
               ? state_->refresh()
               : Result<void>{Status{StatusCode::InvalidState, "plugin manager is empty"}};
}

Result<std::vector<PluginInfo>> PluginManager::plugins() const {
    return state_ != nullptr ? state_->plugins()
                             : Result<std::vector<PluginInfo>>{
                                   Status{StatusCode::InvalidState, "plugin manager is empty"}};
}

Result<PluginLibrary> PluginManager::load(std::string_view plugin_id) {
    if (state_ == nullptr) {
        return Status{StatusCode::InvalidState, "plugin manager is empty"};
    }
    auto loaded = state_->load(plugin_id, {}, false);
    if (!loaded.ok()) {
        return loaded.status();
    }
    return PluginLibrary{std::move(loaded).value(), state_};
}

Result<PluginLibrary> PluginManager::load(
    std::string_view plugin_id, std::string_view plugin_version) {
    if (state_ == nullptr) {
        return Status{StatusCode::InvalidState, "plugin manager is empty"};
    }
    auto loaded = state_->load(plugin_id, plugin_version, true);
    if (!loaded.ok()) {
        return loaded.status();
    }
    return PluginLibrary{std::move(loaded).value(), state_};
}

Result<PluginDiagnostics> PluginManager::diagnostics() const {
    return state_ != nullptr ? state_->diagnostics()
                             : Result<PluginDiagnostics>{
                                   Status{StatusCode::InvalidState, "plugin manager is empty"}};
}

Result<void> PluginManager::shutdown(Deadline deadline) {
    if (managed_call_active() || interface_support::active_operation_owner != nullptr) {
        return Status{
            StatusCode::InvalidState,
            "cannot synchronously shut down from a managed plugin operation"};
    }
    return state_ != nullptr
               ? state_->shutdown(deadline)
               : Result<void>{Status{StatusCode::InvalidState, "plugin manager is empty"}};
}

PluginLibrary::PluginLibrary(
    std::shared_ptr<PluginLibraryState> state, std::weak_ptr<PluginManagerState> manager) noexcept
: state_(std::move(state)), manager_(std::move(manager)) {}

PluginLibrary::PluginLibrary(PluginLibrary&&) noexcept = default;
PluginLibrary& PluginLibrary::operator=(PluginLibrary&&) noexcept = default;
PluginLibrary::~PluginLibrary() noexcept = default;

PluginLibrary::operator bool() const noexcept { return state_ != nullptr; }

const PluginInfo& PluginLibrary::info() const noexcept {
    static const PluginInfo kEmptyInfo;
    return state_ != nullptr ? state_->info() : kEmptyInfo;
}

const PluginArtifactInfo& PluginLibrary::artifact() const noexcept {
    static const PluginArtifactInfo kEmptyArtifact;
    return state_ != nullptr ? state_->artifact_info() : kEmptyArtifact;
}

Result<PluginInstance> PluginLibrary::create_instance(
    const ServiceRegistry& services, std::string_view config) const {
    if (state_ == nullptr) {
        return Status{StatusCode::InvalidState, "plugin library is empty"};
    }
    if (!services.frozen()) {
        return Status{StatusCode::InvalidState, "service registry is not frozen"};
    }
    auto manager = manager_.lock();
    if (manager == nullptr) {
        return Status{StatusCode::InvalidState, "plugin manager is unavailable"};
    }
    auto instance = manager->create_instance(state_, services.backing(), config);
    if (!instance.ok()) {
        return instance.status();
    }
    return PluginInstance{std::move(instance).value()};
}

}  // namespace rodaq::plugin
