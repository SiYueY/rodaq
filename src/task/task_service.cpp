#include "task_service.hpp"

#include "config.hpp"
#include "config_identity.hpp"
#include "create_store.hpp"
#include "identity.hpp"
#include "task_controller.hpp"
#include "../pipeline/stream_identity_store.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <set>

#include <rodaq/plugin/abi.hpp>
#include <rodaq/plugin/version.hpp>

namespace rodaq {
namespace {

namespace fs = std::filesystem;

WallTime wall_now() {
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
}

template <typename T>
struct Reply {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<Result<T>> result;

    void complete(Result<T> value) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        result.emplace(std::move(value));
        ready.notify_all();
    }

    Result<T> wait(std::chrono::nanoseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!ready.wait_for(lock, timeout, [&] { return result.has_value(); })) {
            return Status(StatusCode::DeadlineExceeded, {});
        }
        return std::move(*result);
    }
};

class RequestSlot {
public:
    explicit RequestSlot(std::atomic<std::size_t>& requests) noexcept : requests_(&requests) {}
    ~RequestSlot() noexcept { requests_->fetch_sub(1, std::memory_order_release); }
    RequestSlot(const RequestSlot&) = delete;
    RequestSlot& operator=(const RequestSlot&) = delete;

private:
    std::atomic<std::size_t>* requests_;
};

struct PublishedTask {
    TaskSnapshot snapshot;
    std::shared_ptr<const task::TaskControlRecord> control;
    std::shared_ptr<const TaskConfig> config;
};

struct TaskEntry {
    TaskId id;
    std::mutex control_mutex;
    std::unique_ptr<task::TaskStore> store;
    std::unique_ptr<pipeline::StreamIdentityStore> identities;
    pipeline::StreamIdentityState identity;
    std::unique_ptr<task::TaskController> controller;
    std::shared_ptr<const PublishedTask> published;
    bool quiescent{false};
};

Result<std::shared_ptr<const PublishedTask>> publication(
    const TaskEntry& entry, std::shared_ptr<const TaskConfig> config, std::uint64_t sequence) {
    try {
        auto control = entry.controller->durable_record();
        TaskSnapshot snapshot;
        snapshot.task_id = entry.id;
        snapshot.active_config_version = control->active_config_version;
        snapshot.desired_state = control->desired_state;
        snapshot.control_generation = control->control_generation;
        snapshot.reset_generation = control->reset_generation;
        snapshot.snapshot_sequence = sequence;
        snapshot.captured_at = wall_now();
        snapshot.execution_condition =
            entry.quiescent ? ExecutionCondition::Idle : ExecutionCondition::Protected;
        if (control->current_operation_id) {
            for (const auto& operation : control->operation_history.entries) {
                if (operation.snapshot.operation_id == *control->current_operation_id) {
                    snapshot.current_operation = operation.snapshot;
                }
            }
        }
        return std::make_shared<const PublishedTask>(
            PublishedTask{std::move(snapshot), std::move(control), std::move(config)});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

bool label(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128 &&
           std::all_of(value.begin(), value.end(), [](unsigned char byte) {
               return byte >= 0x21 && byte <= 0x7e;
           });
}

}  // namespace

struct TaskService::State {
    std::shared_ptr<const task::ServiceOptions> options;
    std::vector<plugin::PluginInfo> installed;
    std::unique_ptr<task::TaskCreateStore> creates;
    std::mutex catalog_mutex;
    std::vector<std::shared_ptr<TaskEntry>> entries;
    std::atomic<std::size_t> requests{0};
    std::atomic<bool> accepting{true};
    bool reconcile_creates{false};  // Create lane only.

    persistence::FileFaultHook file_hook(task::ServiceFileDomain domain) const {
        return [policy = options, domain](auto step) {
            return policy->file_faults ? policy->file_faults(domain, step) : 0;
        };
    }

    Result<void> step(task::MaterializationStep value) const {
        if (options->materialization_faults && options->materialization_faults(value) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        return {};
    }

    Result<void> directory(const fs::path& path) const {
        std::error_code error;
        fs::create_directory(path, error);
        if (error || fs::symlink_status(path).type() != fs::file_type::directory) {
            return Status(StatusCode::Unavailable, {});
        }
        return persistence::sync_directory(
            path.parent_path(), file_hook(task::ServiceFileDomain::Directories));
    }

    Result<void> validate_deployment(const TaskConfig& config) const {
        for (const auto& pipeline : config.pipelines) {
            if (std::find(
                    options->resource_profiles.begin(), options->resource_profiles.end(),
                    pipeline.resource_profile) == options->resource_profiles.end() ||
                std::find(
                    options->output_roots.begin(), options->output_roots.end(),
                    pipeline.output_root) == options->output_roots.end()) {
                return Status(StatusCode::PermissionDenied, {});
            }
            const auto allowed = [&](const PluginConfig& selected, task::AdmissionRole role) {
                const bool installed_exact =
                    std::any_of(installed.begin(), installed.end(), [&](const auto& plugin) {
                        return plugin.plugin_id == selected.plugin_id &&
                               plugin.plugin_version == selected.plugin_version;
                    });
                return installed_exact &&
                       std::any_of(
                           options->allowed_plugins.begin(), options->allowed_plugins.end(),
                           [&](const auto& plugin) {
                               return plugin.plugin_id == selected.plugin_id &&
                                      plugin.plugin_version == selected.plugin_version &&
                                      plugin.role == role;
                           });
            };
            if (!allowed(pipeline.acquisition, task::AdmissionRole::Acquisition) ||
                !allowed(pipeline.storage, task::AdmissionRole::Storage)) {
                return Status(StatusCode::PermissionDenied, {});
            }
        }
        return {};
    }

    std::shared_ptr<TaskEntry> find(TaskId id) {
        std::lock_guard<std::mutex> lock(catalog_mutex);
        const auto found = std::lower_bound(
            entries.begin(), entries.end(), id,
            [](const auto& entry, TaskId key) { return entry->id < key; });
        return found != entries.end() && (*found)->id == id ? *found : nullptr;
    }

    Result<void> insert(std::shared_ptr<TaskEntry> entry) {
        std::lock_guard<std::mutex> lock(catalog_mutex);
        const auto position = std::lower_bound(
            entries.begin(), entries.end(), entry->id,
            [](const auto& current, TaskId key) { return current->id < key; });
        if (position != entries.end() && (*position)->id == entry->id) {
            return {};
        }
        if (entries.size() == options->max_tasks) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        entries.insert(position, std::move(entry));  // Capacity was reserved at startup.
        return {};
    }

    Result<std::shared_ptr<TaskEntry>> materialize(TaskId id, const task::AssignedConfig* initial) {
        const auto task_directory = options->root / "tasks" / to_string(id);
        if (initial) {
            auto made = directory(task_directory);
            if (!made.ok()) {
                return made.status();
            }
            made = directory(task_directory / "configs");
            if (!made.ok()) {
                return made.status();
            }
        }
        auto store = task::TaskStore::open(
            task_directory, options->root / "control", id,
            initial ? task::TaskStoreMode::Materializing : task::TaskStoreMode::Existing,
            options->store, file_hook(task::ServiceFileDomain::Task));
        if (!store.ok()) {
            return store.status();
        }
        auto identities = pipeline::StreamIdentityStore::open(
            options->root / "identities", id,
            initial ? pipeline::IdentityStoreMode::Materializing
                    : pipeline::IdentityStoreMode::Existing,
            options->max_stream_identities, file_hook(task::ServiceFileDomain::StreamIdentity));
        if (!identities.ok()) {
            return identities.status();
        }
        if (initial) {
            auto created = store.value()->create_config(initial->config);
            if (!created.ok()) {
                return created.status();
            }
            auto checked = step(task::MaterializationStep::Config);
            if (!checked.ok()) {
                return checked.status();
            }
            auto projection = task::identity_projection(*initial, options->store.config);
            if (!projection.ok()) {
                return projection.status();
            }
            auto initialized = identities.value()->initialize(projection.value());
            if (!initialized.ok()) {
                return initialized.status();
            }
            checked = step(task::MaterializationStep::Identity);
            if (!checked.ok()) {
                return checked.status();
            }
            task::TaskControlRecord candidate;
            candidate.task_id = id;
            candidate.active_config_version = created.value().version;
            auto actual = store.value()->load_control();
            if (!actual.ok() && actual.status().code() == StatusCode::NotFound) {
                auto published = store.value()->publish_control(candidate, 0);
                if (!published.ok()) {
                    return published.status();
                }
            } else if (!actual.ok()) {
                return actual.status();
            } else {
                auto old = task::encode_control_record(actual.value(), options->store.history);
                auto expected = task::encode_control_record(candidate, options->store.history);
                if (!old.ok() || !expected.ok() || old.value() != expected.value()) {
                    return Status(
                        StatusCode::DataLoss, "Half-created Task has foreign Control authority");
                }
            }
            checked = step(task::MaterializationStep::Control);
            if (!checked.ok()) {
                return checked.status();
            }
        }
        auto control = store.value()->load_control();
        if (!control.ok()) {
            return control.status();
        }
        auto config = store.value()->load_config(control.value().active_config_version);
        if (!config.ok()) {
            return config.status();
        }
        auto identity = identities.value()->load();
        if (!identity.ok()) {
            return identity.status();
        }
        task::AssignedConfig selected{
            config.value().config, identity.value().pipeline_id_high_water,
            identity.value().stream_id_high_water};
        auto projection = task::identity_projection(selected, options->store.config);
        if (!projection.ok()) {
            return projection.status();
        }
        auto applied = identities.value()->apply_config_change(
            projection.value(), identity.value().record_revision);
        if (!applied.ok()) {
            return applied.status();
        }
        identity = identities.value()->load();
        if (!identity.ok()) {
            return identity.status();
        }
        auto controller = task::TaskController::create(control.value(), options->store.history);
        if (!controller.ok()) {
            return controller.status();
        }
        auto entry = std::make_shared<TaskEntry>();
        entry->id = id;
        entry->store = std::move(store.value());
        entry->identities = std::move(identities.value());
        entry->identity = std::move(identity.value());
        entry->controller = std::move(controller.value());
        entry->quiescent =
            !control.value().operation_history.history_truncated &&
            control.value().desired_state == DesiredState::Stopped &&
            std::none_of(
                control.value().operation_history.entries.begin(),
                control.value().operation_history.entries.end(), [](const auto& operation) {
                    return operation.snapshot.kind == OperationKind::Start ||
                           operation.snapshot.kind == OperationKind::Resume;
                });
        auto published = publication(
            *entry, std::make_shared<const TaskConfig>(std::move(config.value().config)), 1);
        if (!published.ok()) {
            return published.status();
        }
        entry->published = std::move(published.value());
        return entry;
    }

    Result<TaskId> create(const TaskConfig& config, const RequestIdentity& request) {
        if (reconcile_creates) {
            auto reconciled = creates->reconcile();
            if (!reconciled.ok()) {
                return reconciled.status();
            }
            reconcile_creates = false;
        }
        auto digest = task::create_request_digest(config, options->store.config);
        auto key = task::request_key_hash(request);
        if (!digest.ok()) {
            return digest.status();
        }
        if (!key.ok()) {
            return key.status();
        }
        auto known = creates->lookup(key.value(), digest.value());
        if (!known.ok()) {
            reconcile_creates = true;
            return known.status();
        }
        if (!known.value() || known.value()->phase != task::CreatePhase::Completed) {
            auto allowed = validate_deployment(config);
            if (!allowed.ok()) {
                return allowed.status();
            }
        }
        if (!known.value()) {
            auto candidate = task::new_task_id();
            if (!candidate.ok()) {
                return candidate.status();
            }
            auto claimed =
                creates->claim(key.value(), digest.value(), candidate.value(), wall_now());
            if (!claimed.ok()) {
                reconcile_creates = true;
                return claimed.status();
            }
            known.value() = std::move(claimed.value().receipt);
        }
        auto receipt = std::move(*known.value());
        if (receipt.phase == task::CreatePhase::Completed) {
            if (!find(receipt.assigned_task_id)) {
                auto entry = materialize(receipt.assigned_task_id, nullptr);
                if (!entry.ok()) {
                    return entry.status();
                }
                auto added = insert(std::move(entry.value()));
                if (!added.ok()) {
                    return added.status();
                }
            }
            return receipt.assigned_task_id;
        }
        auto checked = step(task::MaterializationStep::Allocated);
        if (!checked.ok()) {
            return checked.status();
        }
        if (receipt.phase == task::CreatePhase::Allocated) {
            receipt.phase = task::CreatePhase::Materializing;
            auto published = creates->publish(receipt);
            if (!published.ok()) {
                reconcile_creates = true;
                return published.status();
            }
        }
        checked = step(task::MaterializationStep::Materializing);
        if (!checked.ok()) {
            return checked.status();
        }
        auto assigned = task::assign_initial_config(config, options->store.config);
        if (!assigned.ok()) {
            return assigned.status();
        }
        auto entry = materialize(receipt.assigned_task_id, &assigned.value());
        if (!entry.ok()) {
            return entry.status();
        }
        receipt.phase = task::CreatePhase::Completed;
        auto published = creates->publish(receipt);
        if (!published.ok()) {
            reconcile_creates = true;
            return published.status();
        }
        auto added = insert(std::move(entry.value()));
        if (!added.ok()) {
            return added.status();
        }
        checked = step(task::MaterializationStep::Completed);
        if (!checked.ok()) {
            return checked.status();
        }
        return receipt.assigned_task_id;
    }

    struct CreateWork final : Work {
        std::shared_ptr<State> owner;
        TaskConfig config;
        RequestIdentity request;
        std::shared_ptr<Reply<TaskId>> reply;

        void run() noexcept override {
            try {
                reply->complete(owner->create(config, request));
            } catch (const std::bad_alloc&) {
                reply->complete(Status(StatusCode::ResourceExhausted, {}));
            } catch (const fs::filesystem_error&) {
                reply->complete(Status(StatusCode::Unavailable, {}));
            } catch (...) {
                reply->complete(Status(StatusCode::Internal, {}));
            }
        }
    };
};

TaskService::TaskService(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

TaskService::~TaskService() noexcept { state_->accepting.store(false, std::memory_order_release); }

Result<std::unique_ptr<TaskService>> TaskService::open(task::ServiceOptions options) {
    if (!options.plugin_manager || !options.create_lane || options.max_tasks == 0 ||
        options.max_tasks > 1024 || options.max_requests == 0 || options.max_requests > 4096 ||
        options.max_stream_identities == 0 || options.max_stream_identities > 65536 ||
        options.request_wait <= std::chrono::nanoseconds::zero() ||
        options.request_wait > std::chrono::hours{24} || options.resource_profiles.empty() ||
        options.output_roots.empty() || options.allowed_plugins.empty()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        const auto lane = options.create_lane->snapshot();
        if (lane.worker_count != 1 || !lane.accepting ||
            fs::symlink_status(options.root).type() != fs::file_type::directory) {
            return Status(StatusCode::InvalidArgument, {});
        }
        const auto valid_labels = [](const auto& values) {
            if (values.size() > 256) {
                return false;
            }
            std::set<std::string_view> seen;
            for (const auto& value : values) {
                if (!label(value) || !seen.insert(value).second) {
                    return false;
                }
            }
            return true;
        };
        if (!valid_labels(options.resource_profiles) || !valid_labels(options.output_roots) ||
            options.allowed_plugins.size() > 512 || options.store.max_config_versions == 0 ||
            options.store.max_config_versions > 4096 ||
            !task::validate_history({}, options.store.history).ok()) {
            return Status(StatusCode::InvalidArgument, {});
        }
        for (std::size_t i = 0; i < options.allowed_plugins.size(); ++i) {
            const auto& pin = options.allowed_plugins[i];
            if (!plugin::abi::valid_identifier(pin.plugin_id) || pin.plugin_version.size() > 128 ||
                !plugin::validate_plugin_version(pin.plugin_version).ok() ||
                (pin.role != task::AdmissionRole::Acquisition &&
                 pin.role != task::AdmissionRole::Storage) ||
                std::any_of(
                    options.allowed_plugins.begin(),
                    options.allowed_plugins.begin() + static_cast<std::ptrdiff_t>(i),
                    [&](const auto& old) {
                        return old.plugin_id == pin.plugin_id &&
                               old.plugin_version == pin.plugin_version && old.role == pin.role;
                    })) {
                return Status(StatusCode::InvalidArgument, {});
            }
        }
        auto installed = options.plugin_manager->plugins();
        if (!installed.ok()) {
            return installed.status();
        }
        auto state = std::make_shared<State>();
        state->options = std::make_shared<const task::ServiceOptions>(std::move(options));
        state->installed = std::move(installed.value());
        state->entries.reserve(state->options->max_tasks);
        for (const auto* name : {"tasks", "control", "identities", "create"}) {
            auto made = state->directory(state->options->root / name);
            if (!made.ok()) {
                return made.status();
            }
        }
        auto creates = task::TaskCreateStore::open(
            state->options->root / "create", state->options->max_tasks,
            state->file_hook(task::ServiceFileDomain::CreateReceipts));
        if (!creates.ok()) {
            return creates.status();
        }
        state->creates = std::move(creates.value());
        auto receipts = state->creates->retained_receipts();
        if (!receipts.ok()) {
            return receipts.status();
        }
        for (const auto& receipt : receipts.value()) {
            if (receipt.phase == task::CreatePhase::Completed) {
                auto entry = state->materialize(receipt.assigned_task_id, nullptr);
                if (!entry.ok()) {
                    return entry.status();
                }
                auto added = state->insert(std::move(entry.value()));
                if (!added.ok()) {
                    return added.status();
                }
            }
        }
        return std::unique_ptr<TaskService>(new TaskService(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const fs::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<TaskId> TaskService::create_task(const TaskConfig& config, const RequestIdentity& request) {
    if (!state_->accepting.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, {});
    }
    auto count = state_->requests.load(std::memory_order_relaxed);
    do {
        if (count == state_->options->max_requests) {
            return Status(StatusCode::ResourceExhausted, {});
        }
    } while (!state_->requests.compare_exchange_weak(count, count + 1, std::memory_order_acquire));
    RequestSlot slot(state_->requests);
    try {
        auto normalized = task::normalize_config(config, state_->options->store.config);
        if (!normalized.ok()) {
            return normalized.status();
        }
        auto valid_request = task::validate_request_identity(request);
        if (!valid_request.ok()) {
            return valid_request.status();
        }
        auto reply = std::make_shared<Reply<TaskId>>();
        auto work = std::make_shared<State::CreateWork>();
        work->owner = state_;
        work->config = std::move(normalized.value());
        work->request = request;
        work->reply = reply;
        auto submitted = state_->options->create_lane->submit(std::move(work));
        if (!submitted.ok()) {
            return submitted.status();
        }
        return reply->wait(state_->options->request_wait);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<TaskSnapshot> TaskService::get_task(TaskId id) const {
    auto entry = state_->find(id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    try {
        return std::atomic_load(&entry->published)->snapshot;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<TaskConfig> TaskService::get_active_config(TaskId id) const {
    auto entry = state_->find(id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    try {
        return *std::atomic_load(&entry->published)->config;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<OperationSnapshot> TaskService::get_operation(TaskId id, OperationId operation) const {
    auto entry = state_->find(id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    try {
        const auto published = std::atomic_load(&entry->published);
        for (const auto& item : published->control->operation_history.entries) {
            if (item.snapshot.operation_id == operation) {
                return item.snapshot;
            }
        }
        return Status(StatusCode::NotFound, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<OperationPage> TaskService::list_operations(TaskId id, const PageRequest& request) const {
    auto entry = state_->find(id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    const auto published = std::atomic_load(&entry->published);
    return task::operation_page(id, published->control->operation_history, request);
}

Result<TaskPage> TaskService::list_tasks(const PageRequest& request) const {
    if (request.limit == 0 || request.limit > 256 || request.cursor.size() > 64) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        std::optional<TaskId> after;
        if (!request.cursor.empty()) {
            constexpr std::string_view prefix = "tasks:v1:";
            if (request.cursor.substr(0, prefix.size()) != prefix) {
                return Status(StatusCode::InvalidArgument, {});
            }
            auto parsed = parse_task_id(std::string_view(request.cursor).substr(prefix.size()));
            if (!parsed.ok()) {
                return Status(StatusCode::InvalidArgument, {});
            }
            after = parsed.value();
        }
        std::vector<std::shared_ptr<TaskEntry>> selected;
        selected.reserve(request.limit);
        TaskPage page;
        {
            std::lock_guard<std::mutex> lock(state_->catalog_mutex);
            for (const auto& entry : state_->entries) {
                if (after && !(after.value() < entry->id)) {
                    continue;
                }
                if (selected.size() == request.limit) {
                    page.has_more = true;
                    break;
                }
                selected.push_back(entry);
            }
        }
        page.items.reserve(selected.size());
        for (const auto& entry : selected) {
            page.items.push_back(
                {std::atomic_load(&entry->published)->snapshot,
                 {},
                 {},
                 SnapshotFreshness::NotObserved});
        }
        if (page.has_more) {
            page.next_cursor = "tasks:v1:" + to_string(page.items.back().task.task_id);
        }
        return page;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq
