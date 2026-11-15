#include "task_store.hpp"

#include "config.hpp"
#include "../persistence/json.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rodaq::task {
namespace {

std::string config_name(std::string_view version) {
    return std::string(version.substr(7)) + ".json";
}

Result<std::string> control_digest(std::string_view bytes, const HistoryPolicy& policy) {
    auto parsed = persistence::parse_json(
        bytes, {control_record_max_bytes(policy), 6, 256, 20, policy.max_entries,
                32 + policy.max_entries * 20});
    if (!parsed.ok()) {
        return std::move(parsed).status();
    }
    const auto field = parsed.value().find("content_digest");
    if (field == parsed.value().end() || !field->is_string()) {
        return Status(StatusCode::DataLoss, {});
    }
    return field->get_ref<const std::string&>();
}

bool same_request(const StoredOperation& left, const StoredOperation& right) noexcept {
    const auto& a = left.snapshot;
    const auto& b = right.snapshot;
    return left.request.request_key == right.request.request_key &&
           left.request_digest == right.request_digest && a.operation_id == b.operation_id &&
           a.kind == b.kind && a.control_generation == b.control_generation &&
           a.accepted_at == b.accepted_at;
}

Result<void> successor(const TaskControlRecord& previous, const TaskControlRecord& next) {
    if (previous.record_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    if (next.record_revision != previous.record_revision + 1 ||
        next.control_generation < previous.control_generation ||
        next.control_generation - previous.control_generation > 1 ||
        next.reset_generation < previous.reset_generation ||
        next.reset_generation - previous.reset_generation > 1) {
        return Status(StatusCode::Conflict, {});
    }
    const bool new_intent = next.control_generation != previous.control_generation;
    if (!new_intent && (previous.desired_state != next.desired_state ||
                        previous.active_config_version != next.active_config_version ||
                        previous.reset_generation != next.reset_generation ||
                        (next.current_operation_id &&
                         (!previous.current_operation_id ||
                          !(*next.current_operation_id == *previous.current_operation_id))))) {
        return Status(StatusCode::Conflict, {});
    }
    if (new_intent) {
        if (!next.current_operation_id || next.operation_history.entries.empty()) {
            return Status(StatusCode::Conflict, {});
        }
        const auto& newest = next.operation_history.entries.back().snapshot;
        if (previous.active_config_version != next.active_config_version &&
            newest.kind != OperationKind::Configure) {
            return Status(StatusCode::Conflict, {});
        }
        if (newest.kind == OperationKind::Reset &&
            (!next.reset_intent ||
             next.reset_intent->target_control_generation != previous.control_generation)) {
            return Status(StatusCode::Conflict, {});
        }
        if (!(newest.operation_id == *next.current_operation_id) ||
            newest.control_generation != next.control_generation ||
            newest.state != OperationState::Accepted ||
            ((next.reset_generation != previous.reset_generation) !=
             (newest.kind == OperationKind::Reset))) {
            return Status(StatusCode::Conflict, {});
        }
    }
    for (const auto& updated : next.operation_history.entries) {
        const auto found = std::find_if(
            previous.operation_history.entries.begin(), previous.operation_history.entries.end(),
            [&](const auto& old) {
                return old.snapshot.operation_id == updated.snapshot.operation_id;
            });
        if (found == previous.operation_history.entries.end()) {
            if (!new_intent || updated.snapshot.control_generation != next.control_generation) {
                return Status(StatusCode::Conflict, {});
            }
        } else if (
            !same_request(*found, updated) ||
            (terminal(found->snapshot.state) &&
             (found->snapshot.state != updated.snapshot.state ||
              found->snapshot.terminal_at != updated.snapshot.terminal_at)) ||
            (found->snapshot.state == OperationState::InProgress &&
             updated.snapshot.state == OperationState::Accepted)) {
            return Status(StatusCode::Conflict, {});
        }
    }
    for (const auto& old : previous.operation_history.entries) {
        const auto retained = std::any_of(
            next.operation_history.entries.begin(), next.operation_history.entries.end(),
            [&](const auto& item) {
                return item.snapshot.operation_id == old.snapshot.operation_id;
            });
        if (!retained && (!terminal(old.snapshot.state) ||
                          (previous.current_operation_id &&
                           old.snapshot.operation_id == *previous.current_operation_id) ||
                          !next.operation_history.history_truncated)) {
            return Status(StatusCode::Conflict, {});
        }
    }
    if (previous.operation_history.history_truncated && !next.operation_history.history_truncated) {
        return Status(StatusCode::Conflict, {});
    }
    return {};
}

}  // namespace

struct TaskStore::State {
    std::filesystem::path task_directory;
    std::filesystem::path configs;
    std::filesystem::path controls;
    std::string control_name;
    TaskId task_id;
    TaskStoreMode mode{TaskStoreMode::Existing};
    TaskStorePolicy policy;
    persistence::FileFaultHook faults;
    std::mutex execution;
    int lease{-1};
    int control_lease{-1};
    bool protected_state{true};
    std::optional<TaskControlRecord> current;
    std::string content_digest;
    std::string retained_control_temporary;
    std::string pending_control_digest;
    std::set<std::string, std::less<>> config_versions;

    ~State() noexcept {
        if (control_lease >= 0) {
            close(control_lease);
        }
        if (lease >= 0) {
            close(lease);
        }
    }

    Result<TaskConfigVersion> read_config(std::string_view version) {
        if (!persistence::valid_digest(version)) {
            return Status(StatusCode::InvalidArgument, {});
        }
        auto bytes =
            persistence::read_file(configs, config_name(version), policy.config.max_config_bytes);
        if (!bytes.ok()) {
            return std::move(bytes).status();
        }
        auto parsed = parse_task_config(bytes.value(), policy.config);
        if (!parsed.ok()) {
            return parsed.status().code() == StatusCode::InvalidArgument
                       ? Status(StatusCode::DataLoss, {})
                       : std::move(parsed).status();
        }
        auto actual_version = config_version(parsed.value(), policy.config);
        if (!actual_version.ok()) {
            return actual_version.status().code() == StatusCode::InvalidArgument
                       ? Status(StatusCode::DataLoss, {})
                       : std::move(actual_version).status();
        }
        if (actual_version.value() != version) {
            return Status(StatusCode::DataLoss, "Immutable Task Config identity mismatch");
        }
        return TaskConfigVersion{std::string(version), std::move(parsed).value()};
    }

    Result<std::optional<TaskControlRecord>> reload() {
        protected_state = true;
        std::set<std::string, std::less<>> versions;
        std::vector<std::filesystem::path> temporaries;
        std::size_t entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(configs)) {
            if (++entries > policy.max_config_versions + 64) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            const auto name = entry.path().filename().string();
            if (persistence::is_temporary_file_name(name)) {
                if (entry.symlink_status().type() != std::filesystem::file_type::regular) {
                    return Status(
                        StatusCode::DataLoss, "Unexpected Task Config temporary file type");
                }
                if (temporaries.size() >= 64) {
                    return Status(StatusCode::ResourceExhausted, {});
                }
                temporaries.push_back(entry.path());
                continue;
            }
            if (versions.size() >= policy.max_config_versions || name.size() != 69 ||
                name.substr(64) != ".json") {
                return Status(StatusCode::DataLoss, "Unreconciled Task Config namespace entry");
            }
            const auto version = "sha256:" + name.substr(0, 64);
            auto config = read_config(version);
            if (!config.ok()) {
                return std::move(config).status();
            }
            auto synced = persistence::sync_file(configs, name, faults);
            if (!synced.ok()) {
                return std::move(synced).status();
            }
            versions.insert(version);
        }
        for (const auto& temporary : temporaries) {
            std::error_code error;
            if (!std::filesystem::remove(temporary, error) || error) {
                return Status(StatusCode::Unavailable, {});
            }
        }
        auto configs_synced = persistence::sync_directory(configs, faults);
        if (!configs_synced.ok()) {
            return std::move(configs_synced).status();
        }
        auto bytes = persistence::read_file(
            controls, control_name, control_record_max_bytes(policy.history));
        std::optional<TaskControlRecord> record;
        std::string digest;
        if (bytes.ok()) {
            auto loaded = decode_control_record(bytes.value(), policy.history);
            if (!loaded.ok()) {
                return std::move(loaded).status();
            }
            if (!(loaded.value().task_id == task_id) ||
                versions.count(loaded.value().active_config_version) == 0) {
                return Status(
                    StatusCode::DataLoss, "Task Control refers to missing or foreign authority");
            }
            auto synced = persistence::sync_file(controls, control_name, faults);
            if (!synced.ok()) {
                return std::move(synced).status();
            }
            auto actual_digest = control_digest(bytes.value(), policy.history);
            if (!actual_digest.ok()) {
                return std::move(actual_digest).status();
            }
            digest = std::move(actual_digest).value();
            if (!current && !pending_control_digest.empty() && digest != pending_control_digest) {
                return Status(StatusCode::DataLoss, "Unexpected initial Task Control authority");
            }
            if (current && digest != content_digest) {
                if (digest != pending_control_digest) {
                    return Status(
                        StatusCode::DataLoss,
                        "Unexpected Task Control authority during reconciliation");
                }
                auto ordered = successor(*current, loaded.value());
                if (!ordered.ok()) {
                    return std::move(ordered).status();
                }
            }
            record = std::move(loaded).value();
        } else if (bytes.status().code() != StatusCode::NotFound) {
            return std::move(bytes).status();
        } else if (current || mode == TaskStoreMode::Existing) {
            return Status(StatusCode::DataLoss, "Task Control authority disappeared");
        }
        if (!retained_control_temporary.empty()) {
            std::error_code error;
            const auto removed =
                std::filesystem::remove(controls / retained_control_temporary, error);
            if (error ||
                (!removed && std::filesystem::exists(controls / retained_control_temporary))) {
                return Status(StatusCode::Unavailable, {});
            }
        }
        auto synced = persistence::sync_directory(controls, faults);
        if (!synced.ok()) {
            return std::move(synced).status();
        }
        // Allocate the result before committing the in-memory authority cache.
        auto result = record;
        current = std::move(record);
        content_digest = std::move(digest);
        config_versions.swap(versions);
        retained_control_temporary.clear();
        pending_control_digest.clear();
        protected_state = false;
        return result;
    }
};

TaskStore::TaskStore(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

TaskStore::~TaskStore() noexcept = default;

Result<std::unique_ptr<TaskStore>> TaskStore::open(
    const std::filesystem::path& task_directory, const std::filesystem::path& control_directory,
    TaskId task_id, TaskStoreMode mode, TaskStorePolicy policy, persistence::FileFaultHook faults) {
    if ((mode != TaskStoreMode::Existing && mode != TaskStoreMode::Materializing) ||
        std::all_of(
            task_id.value.begin(), task_id.value.end(),
            [](std::uint8_t byte) { return byte == 0; }) ||
        policy.max_config_versions == 0 || policy.max_config_versions > 4096 ||
        !validate_history({}, policy.history).ok()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto state = std::make_unique<State>();
        state->task_directory = task_directory;
        state->configs = task_directory / "configs";
        state->controls = control_directory;
        state->control_name = to_string(task_id) + ".json";
        state->task_id = task_id;
        state->mode = mode;
        state->policy = policy;
        state->faults = std::move(faults);
        state->lease = ::open(
            (task_directory / ".task.lock").c_str(),
            O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        struct stat attributes {};
        if (state->lease < 0 || fstat(state->lease, &attributes) != 0 ||
            !S_ISREG(attributes.st_mode)) {
            return Status(StatusCode::Unavailable, {});
        }
        if (flock(state->lease, LOCK_EX | LOCK_NB) != 0) {
            return Status(
                errno == EWOULDBLOCK ? StatusCode::Conflict : StatusCode::Unavailable, {});
        }
        // The config namespace and the actual (control directory, Task ID)
        // authority are both exclusive. A different task-directory argument
        // cannot accidentally create a second publisher of the same record.
        state->control_lease = ::open(
            (control_directory / (to_string(task_id) + ".lock")).c_str(),
            O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        if (state->control_lease < 0 || fstat(state->control_lease, &attributes) != 0 ||
            !S_ISREG(attributes.st_mode)) {
            return Status(StatusCode::Unavailable, {});
        }
        if (flock(state->control_lease, LOCK_EX | LOCK_NB) != 0) {
            return Status(
                errno == EWOULDBLOCK ? StatusCode::Conflict : StatusCode::Unavailable, {});
        }
        auto loaded = state->reload();
        if (!loaded.ok()) {
            return std::move(loaded).status();
        }
        return std::unique_ptr<TaskStore>(new TaskStore(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<TaskConfigVersion> TaskStore::create_config(const TaskConfig& config) {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    try {
        auto version = config_version(config, state_->policy.config);
        if (!version.ok()) {
            return std::move(version).status();
        }
        if (state_->config_versions.count(version.value()) != 0) {
            return state_->read_config(version.value());
        }
        if (state_->config_versions.size() >= state_->policy.max_config_versions) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        auto normalized = normalize_config(config, state_->policy.config);
        if (!normalized.ok()) {
            return std::move(normalized).status();
        }
        auto bytes = export_task_config(normalized.value(), state_->policy.config);
        if (!bytes.ok()) {
            return std::move(bytes).status();
        }
        TaskConfigVersion response{version.value(), std::move(normalized).value()};
        std::set<std::string, std::less<>> reserved{version.value()};
        auto node = reserved.extract(reserved.begin());
        auto publication = persistence::publish_file(
            state_->configs, config_name(version.value()), bytes.value(),
            persistence::FilePublishMode::Create, state_->policy.config.max_config_bytes,
            state_->faults);
        if (publication.disposition != persistence::FileDisposition::Durable) {
            state_->protected_state = true;
            return std::move(publication.status);
        }
        state_->config_versions.insert(std::move(node));
        return response;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<TaskConfigVersion> TaskStore::load_config(std::string_view version) {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    try {
        return state_->read_config(version);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<TaskControlRecord> TaskStore::load_control() {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    try {
        auto bytes = persistence::read_file(
            state_->controls, state_->control_name,
            control_record_max_bytes(state_->policy.history));
        if (!bytes.ok()) {
            if (state_->current) {
                state_->protected_state = true;
                return Status(StatusCode::DataLoss, "Task Control authority disappeared");
            }
            return std::move(bytes).status();
        }
        auto record = decode_control_record(bytes.value(), state_->policy.history);
        if (!record.ok()) {
            state_->protected_state = true;
            return std::move(record).status();
        }
        auto digest = control_digest(bytes.value(), state_->policy.history);
        if (!digest.ok()) {
            return std::move(digest).status();
        }
        if (!(record.value().task_id == state_->task_id) ||
            digest.value() != state_->content_digest) {
            state_->protected_state = true;
            return Status(
                StatusCode::DataLoss, "Task Control changed outside its publication order");
        }
        return record;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> TaskStore::publish_control(
    const TaskControlRecord& record, std::uint64_t expected_revision) {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    try {
        if (!(record.task_id == state_->task_id) ||
            expected_revision != (state_->current ? state_->current->record_revision : 0)) {
            return Status(StatusCode::Conflict, {});
        }
        auto valid_record = validate_control_record(record, state_->policy.history);
        if (!valid_record.ok()) {
            return std::move(valid_record).status();
        }
        if (state_->current) {
            auto ordered = successor(*state_->current, record);
            if (!ordered.ok()) {
                return std::move(ordered).status();
            }
            auto actual = persistence::read_file(
                state_->controls, state_->control_name,
                control_record_max_bytes(state_->policy.history));
            if (!actual.ok()) {
                state_->protected_state = true;
                return std::move(actual).status();
            }
            auto valid = decode_control_record(actual.value(), state_->policy.history);
            auto digest = control_digest(actual.value(), state_->policy.history);
            if (!valid.ok() || !digest.ok() || digest.value() != state_->content_digest) {
                state_->protected_state = true;
                return Status(
                    StatusCode::DataLoss, "Task Control changed outside its publication order");
            }
        } else if (
            record.record_revision != 1 || record.control_generation != 1 ||
            record.reset_generation != 0 || record.desired_state != DesiredState::Stopped ||
            record.current_operation_id || !record.operation_history.entries.empty() ||
            record.reset_intent || record.operation_history.history_truncated) {
            return Status(StatusCode::InvalidArgument, {});
        }
        if (state_->config_versions.count(record.active_config_version) == 0) {
            return Status(StatusCode::NotFound, "Active Config has no durable publication proof");
        }
        auto config = state_->read_config(record.active_config_version);
        if (!config.ok()) {
            return std::move(config).status();
        }
        auto bytes = encode_control_record(record, state_->policy.history);
        if (!bytes.ok()) {
            return std::move(bytes).status();
        }
        auto digest = control_digest(bytes.value(), state_->policy.history);
        if (!digest.ok()) {
            return std::move(digest).status();
        }
        // All owned results are allocated before publication can have effects.
        std::optional<TaskControlRecord> candidate{record};
        auto publication = persistence::publish_file(
            state_->controls, state_->control_name, bytes.value(),
            state_->current ? persistence::FilePublishMode::Replace
                            : persistence::FilePublishMode::Create,
            control_record_max_bytes(state_->policy.history), state_->faults);
        if (publication.disposition != persistence::FileDisposition::Durable) {
            state_->protected_state = true;
            state_->retained_control_temporary = std::move(publication.retained_temporary);
            state_->pending_control_digest = std::move(digest).value();
            return std::move(publication.status);
        }
        state_->current = std::move(candidate);
        state_->content_digest = std::move(digest).value();
        state_->pending_control_digest.clear();
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::optional<TaskControlRecord>> TaskStore::reconcile() {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    try {
        return state_->reload();
    } catch (const std::bad_alloc&) {
        state_->protected_state = true;
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        state_->protected_state = true;
        return Status(StatusCode::Unavailable, {});
    }
}

}  // namespace rodaq::task
