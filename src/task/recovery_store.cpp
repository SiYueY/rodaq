#include "recovery_store.hpp"

#include "../persistence/file_descriptor.hpp"
#include "../uuid.hpp"

#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <utility>

namespace rodaq::task {

struct TaskRecoveryStore::State {
    explicit State(persistence::FileDescriptor lease_value) : lease(std::move(lease_value)) {}

    persistence::FileDescriptor lease;
    std::filesystem::path directory;
    std::string name;
    TaskId task_id;
    RecoveryPolicy policy;
    persistence::FileFaultHook faults;
    std::mutex execution;
    std::optional<TaskRecoveryRecord> current;
    std::string current_bytes;
    std::string pending_bytes;
    std::string retained_temporary;
    bool protected_state{true};
    std::chrono::steady_clock::time_point window_start{std::chrono::steady_clock::now()};
    std::uint32_t publications{0};

    Result<std::optional<TaskRecoveryRecord>> reload() {
        protected_state = true;
        auto bytes = persistence::read_file(directory, name, policy.limits.max_bytes);
        std::optional<TaskRecoveryRecord> loaded;
        std::string canonical;
        if (bytes.ok()) {
            auto record = decode_recovery_record(bytes.value(), policy.limits);
            if (!record.ok()) {
                return record.status();
            }
            if (!(record.value().task_id == task_id)) {
                return Status(StatusCode::DataLoss, {});
            }
            auto encoded = encode_recovery_record(record.value(), policy.limits);
            if (!encoded.ok()) {
                return encoded.status();
            }
            canonical = std::move(encoded.value());
            if ((!current_bytes.empty() || !pending_bytes.empty()) && canonical != current_bytes &&
                canonical != pending_bytes) {
                return Status(StatusCode::DataLoss, "Unrecognized Recovery publication");
            }
            auto synced = persistence::sync_file(directory, name, faults);
            if (!synced.ok()) {
                return synced.status();
            }
            loaded = std::move(record.value());
        } else if (bytes.status().code() != StatusCode::NotFound) {
            return bytes.status();
        } else if (current) {
            return Status(StatusCode::DataLoss, "Recovery authority disappeared");
        }
        if (!retained_temporary.empty()) {
            std::error_code error;
            std::filesystem::remove(directory / retained_temporary, error);
            if (error) {
                return Status(StatusCode::Unavailable, {});
            }
        }
        auto synced = persistence::sync_directory(directory, faults);
        if (!synced.ok()) {
            return synced.status();
        }
        auto result = loaded;
        current = std::move(loaded);
        current_bytes.swap(canonical);
        pending_bytes.clear();
        retained_temporary.clear();
        protected_state = false;
        return result;
    }

    Result<void> verify_current() {
        auto bytes = persistence::read_file(directory, name, policy.limits.max_bytes);
        if (!bytes.ok()) {
            if (!current && bytes.status().code() == StatusCode::NotFound) {
                return {};
            }
            protected_state = true;
            return Status(StatusCode::DataLoss, "Recovery authority missing or unreadable");
        }
        auto decoded = decode_recovery_record(bytes.value(), policy.limits);
        if (!decoded.ok()) {
            protected_state = true;
            return decoded.status();
        }
        auto canonical = encode_recovery_record(decoded.value(), policy.limits);
        if (!canonical.ok()) {
            return canonical.status();
        }
        if (canonical.value() != current_bytes) {
            protected_state = true;
            return Status(StatusCode::DataLoss, "Recovery changed outside its owner");
        }
        return {};
    }
};

TaskRecoveryStore::TaskRecoveryStore(std::unique_ptr<State> state) noexcept
: state_(std::move(state)) {}

TaskRecoveryStore::~TaskRecoveryStore() noexcept = default;

Result<std::unique_ptr<TaskRecoveryStore>> TaskRecoveryStore::open(
    const std::filesystem::path& directory, TaskId task_id, RecoveryPolicy policy,
    persistence::FileFaultHook faults) {
    if (!uuid::nonzero(task_id.value) || !valid_recovery_limits(policy.limits) ||
        policy.max_publications_per_window == 0 || policy.max_publications_per_window > 65536 ||
        policy.publication_window <= std::chrono::nanoseconds::zero() ||
        policy.publication_window > std::chrono::hours{24}) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        const auto name = to_string(task_id);
        persistence::FileDescriptor lease(::open(
            (directory / (name + ".lock")).c_str(),
            O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
        struct stat attributes {};
        if (lease.get() < 0 || fstat(lease.get(), &attributes) != 0 ||
            !S_ISREG(attributes.st_mode)) {
            return Status(StatusCode::Unavailable, {});
        }
        if (flock(lease.get(), LOCK_EX | LOCK_NB) != 0) {
            return Status(
                errno == EWOULDBLOCK ? StatusCode::Conflict : StatusCode::Unavailable, {});
        }
        auto state = std::make_unique<State>(std::move(lease));
        state->directory = directory;
        state->name = name + ".json";
        state->task_id = task_id;
        state->policy = policy;
        state->faults = std::move(faults);
        auto loaded = state->reload();
        if (!loaded.ok()) {
            return loaded.status();
        }
        return std::unique_ptr<TaskRecoveryStore>(new TaskRecoveryStore(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<TaskRecoveryRecord> TaskRecoveryStore::load() {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    try {
        auto valid = state_->verify_current();
        if (!valid.ok()) {
            return valid.status();
        }
        if (!state_->current) {
            return Status(StatusCode::NotFound, {});
        }
        return *state_->current;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> TaskRecoveryStore::publish(
    const TaskRecoveryRecord& record, std::uint64_t expected_revision) {
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
        auto bytes = encode_recovery_record(record, state_->policy.limits);
        if (!bytes.ok()) {
            return bytes.status();
        }
        if (state_->current) {
            auto ordered = recovery_successor(*state_->current, record);
            if (!ordered.ok()) {
                return ordered.status();
            }
        } else if (record.record_revision != 1) {
            return Status(StatusCode::Conflict, {});
        }
        auto valid = state_->verify_current();
        if (!valid.ok()) {
            return valid.status();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - state_->window_start >= state_->policy.publication_window) {
            state_->window_start = now;
            state_->publications = 0;
        }
        if (state_->publications == state_->policy.max_publications_per_window) {
            return Status(StatusCode::ResourceExhausted, "Recovery write budget exhausted");
        }
        // Allocate every retained result before the publication can have effects.
        std::optional<TaskRecoveryRecord> candidate{record};
        ++state_->publications;
        auto publication = persistence::publish_file(
            state_->directory, state_->name, bytes.value(),
            state_->current ? persistence::FilePublishMode::Replace
                            : persistence::FilePublishMode::Create,
            state_->policy.limits.max_bytes, state_->faults);
        if (publication.disposition != persistence::FileDisposition::Durable) {
            state_->protected_state = true;
            state_->pending_bytes = std::move(bytes.value());
            state_->retained_temporary = std::move(publication.retained_temporary);
            return std::move(publication.status);
        }
        state_->current = std::move(candidate);
        state_->current_bytes = std::move(bytes.value());
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::optional<TaskRecoveryRecord>> TaskRecoveryStore::reconcile() {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    try {
        return state_->reload();
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::task
