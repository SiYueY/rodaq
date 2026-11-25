#include "retirement_store.hpp"

#include "identity.hpp"
#include "../persistence/decimal.hpp"
#include "../persistence/file_descriptor.hpp"
#include "../persistence/json.hpp"
#include "../time/wall_time.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rodaq::task {
namespace {

constexpr std::string_view kDomain = "rodaq.retired-task.v1";
constexpr std::size_t kMaxBytes = 4096;

persistence::JsonLimits limits() { return {kMaxBytes, 2, 256, 12, 0, 20}; }

struct StoredTombstone {
    TaskRetirementRecord record;
    std::string content_digest;
};

bool nonzero(const std::array<std::uint8_t, 16>& bytes) noexcept {
    return std::any_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte != 0; });
}

std::string file_name(TaskId task_id) { return to_string(task_id) + ".json"; }

int injected(const persistence::FileFaultHook& faults, persistence::FileStep step) noexcept {
    try {
        return faults ? faults(step) : 0;
    } catch (...) {
        return EIO;
    }
}

Result<void> sync_descriptor(
    int descriptor, persistence::FileStep step, const persistence::FileFaultHook& faults) {
    if (injected(faults, step) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    int result = 0;
    do {
        result = fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result == 0 ? Result<void>{} : Result<void>{Status(StatusCode::Unavailable, {})};
}

Result<StoredTombstone> decode_stored(std::string_view bytes) {
    auto parsed = persistence::parse_json(bytes, limits());
    if (!parsed.ok()) {
        return parsed.status().code() == StatusCode::InvalidArgument
                   ? Status(StatusCode::DataLoss, {})
                   : std::move(parsed).status();
    }
    const auto& document = parsed.value();
    if (!document.is_object() || document.size() != 10) {
        return Status(StatusCode::DataLoss, "Invalid retirement tombstone structure");
    }
    for (const auto* field :
         {"format_version", "task_id", "retire_operation_id", "request_key", "request_digest",
          "terminal_result", "last_control_generation", "last_config_version", "retired_at",
          "content_digest"}) {
        const auto found = document.find(field);
        if (found == document.end() || !found->is_string()) {
            return Status(StatusCode::DataLoss, {});
        }
    }
    const auto text = [&](const char* field) -> const std::string& {
        return document.at(field).get_ref<const std::string&>();
    };
    if (text("format_version") != "1.0") {
        return Status(StatusCode::Unavailable, {});
    }
    if (text("terminal_result") != "succeeded") {
        return Status(
            StatusCode::DataLoss, "Retirement tombstone terminal_result must be succeeded");
    }
    auto task_id = parse_task_id(text("task_id"));
    auto operation_id = parse_operation_id(text("retire_operation_id"));
    auto generation = persistence::unsigned_decimal(text("last_control_generation"));
    auto retired_at = time::parse_wall_time(text("retired_at"));
    if (!retired_at.ok() && retired_at.status().code() == StatusCode::ResourceExhausted) {
        return std::move(retired_at).status();
    }
    if (!task_id.ok() || !operation_id.ok() || !generation.ok() || !retired_at.ok()) {
        return Status(StatusCode::DataLoss, "Invalid retirement tombstone identity or timestamp");
    }
    TaskRetirementRecord record{
        task_id.value(),        operation_id.value(), RequestIdentity{text("request_key")},
        text("request_digest"), generation.value(),   text("last_config_version"),
        retired_at.value()};
    auto valid = validate_retirement_record(record);
    if (!valid.ok()) {
        return Status(StatusCode::DataLoss, {});
    }
    auto verified = persistence::verify_integrity(document, kDomain, limits());
    if (!verified.ok()) {
        return std::move(verified).status();
    }
    return StoredTombstone{std::move(record), text("content_digest")};
}

bool same_tombstone(const TaskRetirementRecord& left, const TaskRetirementRecord& right) noexcept {
    return left.task_id == right.task_id && left.retire_operation_id == right.retire_operation_id &&
           left.request.request_key == right.request.request_key &&
           left.request_digest == right.request_digest &&
           left.last_control_generation == right.last_control_generation &&
           left.last_config_version == right.last_config_version;
}

}  // namespace

Result<void> validate_retirement_record(const TaskRetirementRecord& record) {
    if (!nonzero(record.task_id.value) || !nonzero(record.retire_operation_id.value) ||
        !validate_request_identity(record.request).ok() ||
        !persistence::valid_digest(record.request_digest) || record.last_control_generation == 0 ||
        !persistence::valid_digest(record.last_config_version)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto formatted = time::format_wall_time(record.retired_at);
    if (!formatted.ok()) {
        return std::move(formatted).status();
    }
    return {};
}

Result<std::string> encode_retirement_record(const TaskRetirementRecord& record) {
    auto valid = validate_retirement_record(record);
    if (!valid.ok()) {
        return std::move(valid).status();
    }
    try {
        auto timestamp = time::format_wall_time(record.retired_at);
        if (!timestamp.ok()) {
            return std::move(timestamp).status();
        }
        const persistence::Json document{
            {"format_version", "1.0"},
            {"task_id", to_string(record.task_id)},
            {"retire_operation_id", to_string(record.retire_operation_id)},
            {"request_key", record.request.request_key},
            {"request_digest", record.request_digest},
            {"terminal_result", "succeeded"},
            {"last_control_generation", std::to_string(record.last_control_generation)},
            {"last_config_version", record.last_config_version},
            {"retired_at", std::move(timestamp).value()}};
        return persistence::encode_integrity(document, kDomain, limits());
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<TaskRetirementRecord> decode_retirement_record(std::string_view bytes) {
    auto decoded = decode_stored(bytes);
    if (!decoded.ok()) {
        return std::move(decoded).status();
    }
    return std::move(decoded.value().record);
}

OperationSnapshot retirement_operation_snapshot(const TaskRetirementRecord& record) {
    OperationSnapshot snapshot;
    snapshot.operation_id = record.retire_operation_id;
    snapshot.kind = OperationKind::Retire;
    snapshot.state = OperationState::Succeeded;
    snapshot.control_generation = record.last_control_generation;
    snapshot.accepted_at = record.retired_at;
    snapshot.terminal_at = record.retired_at;
    snapshot.retryability = Retryability::Never;
    snapshot.failure_domain = FailureDomain::TaskControl;
    snapshot.data_impact = DataImpact::None;
    snapshot.status_code = StatusCode::Ok;
    return snapshot;
}

struct TaskRetirementStore::State {
    std::filesystem::path directory;
    std::size_t max_tombstones{0};
    persistence::FileFaultHook faults;
    // Serializes filesystem persistence and reconciliation.
    std::mutex io_mutex;
    // Protects the reconciled in-memory cache; never held across disk I/O.
    mutable std::mutex cache_mutex;
    persistence::FileDescriptor lease;
    bool protected_state{true};
    std::map<TaskId, StoredTombstone> tombstones;

    State(
        std::filesystem::path dir, std::size_t limit, persistence::FileFaultHook hook,
        persistence::FileDescriptor lock_fd) noexcept
    : directory(std::move(dir)),
      max_tombstones(limit),
      faults(std::move(hook)),
      lease(std::move(lock_fd)) {}

    Result<void> check_actual_locked(const StoredTombstone& expected) {
        auto bytes =
            persistence::read_file(directory, file_name(expected.record.task_id), kMaxBytes);
        if (!bytes.ok()) {
            std::lock_guard<std::mutex> cache_lock(cache_mutex);
            protected_state = true;
            return std::move(bytes).status();
        }
        auto actual = decode_stored(bytes.value());
        if (!actual.ok()) {
            std::lock_guard<std::mutex> cache_lock(cache_mutex);
            protected_state = true;
            return std::move(actual).status();
        }
        if (actual.value().content_digest != expected.content_digest) {
            std::lock_guard<std::mutex> cache_lock(cache_mutex);
            protected_state = true;
            return Status(StatusCode::DataLoss, "Retirement tombstone changed outside authority");
        }
        return {};
    }

    Result<void> reload_locked() {
        {
            std::lock_guard<std::mutex> cache_lock(cache_mutex);
            protected_state = true;
        }
        std::map<TaskId, StoredTombstone> loaded;
        std::vector<std::string> temporaries;
        std::size_t entries = 0;
        persistence::FileDescriptor directory_fd(
            ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (directory_fd.get() < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (++entries > max_tombstones + 65) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            const auto name = entry.path().filename().string();
            if (name == ".retired.lock") {
                continue;
            }
            if (persistence::is_temporary_file_name(name)) {
                if (entry.symlink_status().type() != std::filesystem::file_type::regular) {
                    return Status(
                        StatusCode::DataLoss, "Unexpected retirement temporary file type");
                }
                if (temporaries.size() >= 64) {
                    return Status(StatusCode::ResourceExhausted, {});
                }
                temporaries.push_back(name);
                continue;
            }
            if (loaded.size() >= max_tombstones || name.size() != 41 ||
                name.substr(36) != ".json") {
                return Status(StatusCode::DataLoss, "Unknown or excessive retired namespace entry");
            }
            auto bytes = persistence::read_file(directory, name, kMaxBytes);
            if (!bytes.ok()) {
                return std::move(bytes).status();
            }
            auto decoded = decode_stored(bytes.value());
            if (!decoded.ok()) {
                return std::move(decoded).status();
            }
            const auto id = decoded.value().record.task_id;
            if (file_name(id) != name) {
                return Status(StatusCode::DataLoss, "Retirement tombstone filename mismatch");
            }
            persistence::FileDescriptor record_fd(openat(
                directory_fd.get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            struct stat record_stat {};
            if (record_fd.get() < 0 || fstat(record_fd.get(), &record_stat) != 0 ||
                !S_ISREG(record_stat.st_mode)) {
                return Status(StatusCode::DataLoss, "Retirement tombstone is not a regular file");
            }
            auto synced = sync_descriptor(record_fd.get(), persistence::FileStep::SyncFile, faults);
            if (!synced.ok()) {
                return std::move(synced).status();
            }
            loaded.emplace(id, std::move(decoded).value());
        }
        for (const auto& name : temporaries) {
            if (injected(faults, persistence::FileStep::RemoveTemporary) != 0 ||
                unlinkat(directory_fd.get(), name.c_str(), 0) != 0) {
                return Status(StatusCode::Unavailable, {});
            }
        }
        auto synced =
            sync_descriptor(directory_fd.get(), persistence::FileStep::SyncDirectory, faults);
        if (!synced.ok()) {
            return std::move(synced).status();
        }
        {
            std::lock_guard<std::mutex> cache_lock(cache_mutex);
            tombstones.swap(loaded);
            protected_state = false;
        }
        return {};
    }
};

TaskRetirementStore::TaskRetirementStore(std::unique_ptr<State> state) noexcept
: state_(std::move(state)) {}

TaskRetirementStore::~TaskRetirementStore() noexcept = default;

Result<std::unique_ptr<TaskRetirementStore>> TaskRetirementStore::open(
    const std::filesystem::path& directory, std::size_t max_tombstones,
    persistence::FileFaultHook faults) {
    if (max_tombstones == 0 || max_tombstones > 65536) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        persistence::FileDescriptor lease(::open(
            (directory / ".retired.lock").c_str(),
            O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
        struct stat lock_stat {};
        if (lease.get() < 0 || fstat(lease.get(), &lock_stat) != 0 || !S_ISREG(lock_stat.st_mode)) {
            return Status(StatusCode::Unavailable, {});
        }
        if (flock(lease.get(), LOCK_EX | LOCK_NB) != 0) {
            return Status(
                errno == EWOULDBLOCK ? StatusCode::Conflict : StatusCode::Unavailable, {});
        }
        auto state =
            std::make_unique<State>(directory, max_tombstones, std::move(faults), std::move(lease));
        auto loaded = state->reload_locked();
        if (!loaded.ok()) {
            return std::move(loaded).status();
        }
        return std::unique_ptr<TaskRetirementStore>(new TaskRetirementStore(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<void> TaskRetirementStore::publish(const TaskRetirementRecord& record) {
    std::lock_guard<std::mutex> io_lock(state_->io_mutex);
    try {
        auto valid = validate_retirement_record(record);
        if (!valid.ok()) {
            return std::move(valid).status();
        }
        std::optional<StoredTombstone> existing;
        {
            std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
            if (state_->protected_state) {
                return Status(StatusCode::Unavailable, {});
            }
            const auto found = state_->tombstones.find(record.task_id);
            if (found != state_->tombstones.end()) {
                existing = found->second;
            } else if (state_->tombstones.size() >= state_->max_tombstones) {
                return Status(StatusCode::ResourceExhausted, {});
            }
        }
        if (existing) {
            auto checked = state_->check_actual_locked(*existing);
            if (!checked.ok()) {
                return std::move(checked).status();
            }
            if (!same_tombstone(existing->record, record)) {
                return Status(StatusCode::Conflict, {});
            }
            return {};
        }
        auto bytes = encode_retirement_record(record);
        if (!bytes.ok()) {
            return std::move(bytes).status();
        }
        auto stored = decode_stored(bytes.value());
        if (!stored.ok()) {
            return std::move(stored).status();
        }
        std::map<TaskId, StoredTombstone> reserved;
        reserved.emplace(record.task_id, std::move(stored).value());
        auto node = reserved.extract(reserved.begin());
        auto published = persistence::publish_file(
            state_->directory, file_name(record.task_id), bytes.value(),
            persistence::FilePublishMode::Create, kMaxBytes, state_->faults);
        if (published.disposition != persistence::FileDisposition::Durable) {
            std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
            state_->protected_state =
                published.disposition == persistence::FileDisposition::Uncertain ||
                !published.retained_temporary.empty() ||
                published.status.code() == StatusCode::Conflict;
            return std::move(published.status);
        }
        std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
        state_->tombstones.insert(std::move(node));
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<TaskRetirementRecord> TaskRetirementStore::load(TaskId task_id) const {
    try {
        std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
        if (state_->protected_state) {
            return Status(StatusCode::Unavailable, {});
        }
        const auto found = state_->tombstones.find(task_id);
        if (found == state_->tombstones.end()) {
            return Status(StatusCode::NotFound, {});
        }
        return found->second.record;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<bool> TaskRetirementStore::contains(TaskId task_id) const {
    std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    return state_->tombstones.count(task_id) != 0;
}

Result<std::vector<TaskRetirementRecord>> TaskRetirementStore::list() const {
    try {
        std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
        if (state_->protected_state) {
            return Status(StatusCode::Unavailable, {});
        }
        std::vector<TaskRetirementRecord> records;
        records.reserve(state_->tombstones.size());
        for (const auto& entry : state_->tombstones) {
            records.push_back(entry.second.record);
        }
        return records;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<OperationId> TaskRetirementStore::lookup_request(
    TaskId task_id, const RequestIdentity& request, std::string_view request_digest) const {
    if (!validate_request_identity(request).ok() || !persistence::valid_digest(request_digest)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    const auto found = state_->tombstones.find(task_id);
    if (found == state_->tombstones.end()) {
        return Status(StatusCode::NotFound, {});
    }
    const auto& record = found->second.record;
    if (record.request.request_key != request.request_key) {
        return Status(StatusCode::NotFound, {});
    }
    if (record.request_digest != request_digest) {
        return Status(StatusCode::Conflict, {});
    }
    return record.retire_operation_id;
}

Result<void> TaskRetirementStore::collect(TaskId task_id, const RetirementCollectProof& proof) {
    if (proof.audit_window <= std::chrono::nanoseconds::zero()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (!proof.active_namespace_removed || !proof.no_artifact_references ||
        !proof.no_pending_maintenance) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> io_lock(state_->io_mutex);
    try {
        std::optional<StoredTombstone> existing;
        {
            std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
            if (state_->protected_state) {
                return Status(StatusCode::Unavailable, {});
            }
            const auto found = state_->tombstones.find(task_id);
            if (found == state_->tombstones.end()) {
                return Status(StatusCode::NotFound, {});
            }
            existing = found->second;
        }
        // Fail-closed audit window expiry check without signed overflow.
        const auto retired_ns = existing->record.retired_at.time_since_epoch().count();
        const auto now_ns = proof.now.time_since_epoch().count();
        if (now_ns < retired_ns) {
            return Status(StatusCode::InvalidState, {});
        }
        const auto elapsed =
            static_cast<std::uint64_t>(now_ns) - static_cast<std::uint64_t>(retired_ns);
        if (elapsed < static_cast<std::uint64_t>(proof.audit_window.count())) {
            return Status(StatusCode::InvalidState, {});
        }
        auto checked = state_->check_actual_locked(*existing);
        if (!checked.ok()) {
            return std::move(checked).status();
        }
        persistence::FileDescriptor directory_fd(
            ::open(state_->directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (directory_fd.get() < 0) {
            std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
            state_->protected_state = true;
            return Status(StatusCode::Unavailable, {});
        }
        const auto name = file_name(task_id);
        if (injected(state_->faults, persistence::FileStep::RemoveAuthority) != 0 ||
            unlinkat(directory_fd.get(), name.c_str(), 0) != 0) {
            std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
            state_->protected_state = true;
            return Status(StatusCode::Unavailable, {});
        }
        auto synced = sync_descriptor(
            directory_fd.get(), persistence::FileStep::SyncDirectory, state_->faults);
        if (!synced.ok()) {
            std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
            state_->protected_state = true;
            return std::move(synced).status();
        }
        std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
        state_->tombstones.erase(task_id);
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<void> TaskRetirementStore::reconcile() {
    std::lock_guard<std::mutex> io_lock(state_->io_mutex);
    try {
        return state_->reload_locked();
    } catch (const std::bad_alloc&) {
        std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
        state_->protected_state = true;
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
        state_->protected_state = true;
        return Status(StatusCode::Unavailable, {});
    }
}

}  // namespace rodaq::task
