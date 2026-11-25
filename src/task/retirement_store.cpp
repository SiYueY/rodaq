#include "retirement_store.hpp"

#include "identity.hpp"
#include "../persistence/decimal.hpp"
#include "../persistence/file_descriptor.hpp"
#include "../persistence/json.hpp"
#include "../time/wall_time.hpp"
#include "../uuid.hpp"

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

namespace rodaq::task {
namespace {

using persistence::FileDescriptor;
using persistence::Json;

constexpr std::string_view kDomain = "rodaq.task-retirement.v1";
constexpr std::size_t kMaxBytes = 8192;

persistence::JsonLimits limits() { return {kMaxBytes, 4, 256, 16, 0, 16}; }

std::string file_name(TaskId id) { return to_string(id) + ".json"; }

Result<void> validate_tombstone(const RetirementTombstone& tombstone) {
    if (!uuid::nonzero(tombstone.task_id.value) || !uuid::nonzero(tombstone.operation_id.value) ||
        tombstone.final_control_generation == 0 ||
        !validate_request_identity({tombstone.request_key}).ok() ||
        !persistence::valid_digest(tombstone.request_digest) ||
        tombstone.retired_at < tombstone.accepted_at) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return {};
}

Result<std::string> encode(const RetirementTombstone& tombstone) {
    auto valid = validate_tombstone(tombstone);
    if (!valid.ok()) {
        return valid.status();
    }
    auto accepted = time::format_wall_time(tombstone.accepted_at);
    auto retired = time::format_wall_time(tombstone.retired_at);
    if (!accepted.ok() || !retired.ok()) {
        return !accepted.ok() ? accepted.status() : retired.status();
    }
    try {
        const Json document{
            {"format_version", "1.0"},
            {"task_id", to_string(tombstone.task_id)},
            {"operation_id", to_string(tombstone.operation_id)},
            {"request_key", tombstone.request_key},
            {"request_digest", tombstone.request_digest},
            {"final_control_generation", std::to_string(tombstone.final_control_generation)},
            {"accepted_at", std::move(accepted.value())},
            {"retired_at", std::move(retired.value())},
            {"cleanup_completed", tombstone.cleanup_completed}};
        return persistence::encode_integrity(document, kDomain, limits());
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<RetirementTombstone> decode(std::string_view bytes) {
    auto parsed = persistence::parse_json(bytes, limits());
    if (!parsed.ok()) {
        return parsed.status().code() == StatusCode::InvalidArgument
                   ? Status(StatusCode::DataLoss, {})
                   : parsed.status();
    }
    const auto& doc = parsed.value();
    if (!doc.is_object() || doc.size() != 10) {
        return Status(StatusCode::DataLoss, {});
    }
    for (const auto* field :
         {"format_version", "task_id", "operation_id", "request_key", "request_digest",
          "final_control_generation", "accepted_at", "retired_at", "content_digest"}) {
        if (!doc.contains(field) || !doc.at(field).is_string()) {
            return Status(StatusCode::DataLoss, {});
        }
    }
    if (!doc.contains("cleanup_completed") || !doc.at("cleanup_completed").is_boolean()) {
        return Status(StatusCode::DataLoss, {});
    }
    if (doc.at("format_version").get_ref<const std::string&>() != "1.0") {
        return Status(StatusCode::Unavailable, {});
    }
    auto verified = persistence::verify_integrity(doc, kDomain, limits());
    if (!verified.ok()) {
        return verified.status();
    }
    auto task_id = parse_task_id(doc.at("task_id").get_ref<const std::string&>());
    auto op_id = parse_operation_id(doc.at("operation_id").get_ref<const std::string&>());
    auto gen = persistence::unsigned_decimal(
        doc.at("final_control_generation").get_ref<const std::string&>());
    auto accepted = time::parse_wall_time(doc.at("accepted_at").get_ref<const std::string&>());
    auto retired = time::parse_wall_time(doc.at("retired_at").get_ref<const std::string&>());
    if (!task_id.ok() || !op_id.ok() || !gen.ok() || !accepted.ok() || !retired.ok()) {
        return Status(StatusCode::DataLoss, {});
    }
    RetirementTombstone tombstone;
    tombstone.task_id = task_id.value();
    tombstone.operation_id = op_id.value();
    tombstone.request_key = doc.at("request_key").get<std::string>();
    tombstone.request_digest = doc.at("request_digest").get<std::string>();
    tombstone.final_control_generation = gen.value();
    tombstone.accepted_at = accepted.value();
    tombstone.retired_at = retired.value();
    tombstone.cleanup_completed = doc.at("cleanup_completed").get<bool>();
    auto valid = validate_tombstone(tombstone);
    if (!valid.ok()) {
        return Status(StatusCode::DataLoss, {});
    }
    return tombstone;
}

bool temporary_name(std::string_view name) noexcept {
    return name.size() == 43 && name.compare(0, 11, ".rodaq-tmp-") == 0 &&
           std::all_of(name.begin() + 11, name.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

}  // namespace

struct TaskRetirementStore::State {
    explicit State(FileDescriptor lock_fd) noexcept : lock(std::move(lock_fd)) {}

    std::filesystem::path directory;
    std::size_t max_tombstones{0};
    persistence::FileFaultHook faults;
    FileDescriptor lock;
    mutable std::mutex mutex;
    std::map<TaskId, RetirementTombstone> tombstones;
};

TaskRetirementStore::TaskRetirementStore(std::unique_ptr<State> state) noexcept
: state_(std::move(state)) {}

TaskRetirementStore::~TaskRetirementStore() noexcept = default;

Result<std::unique_ptr<TaskRetirementStore>> TaskRetirementStore::open(
    const std::filesystem::path& directory, std::size_t max_tombstones,
    persistence::FileFaultHook faults) {
    if (directory.empty() || max_tombstones == 0 || max_tombstones > 65536) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        FileDescriptor dir_fd(
            ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (dir_fd.get() < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        FileDescriptor lock_fd(::openat(
            dir_fd.get(), ".retired.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC,
            0600));
        struct stat lock_stat {};
        if (lock_fd.get() < 0 || ::fstat(lock_fd.get(), &lock_stat) != 0 ||
            !S_ISREG(lock_stat.st_mode)) {
            return Status(StatusCode::DataLoss, {});
        }
        if (::flock(lock_fd.get(), LOCK_EX | LOCK_NB) != 0) {
            return Status(
                errno == EWOULDBLOCK ? StatusCode::Conflict : StatusCode::Unavailable, {});
        }

        auto state = std::make_unique<State>(std::move(lock_fd));
        state->directory = directory;
        state->max_tombstones = max_tombstones;
        state->faults = std::move(faults);

        std::vector<std::string> temporaries;
        std::size_t scanned = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (++scanned > max_tombstones + 65) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            const auto name = entry.path().filename().string();
            if (name == ".retired.lock") {
                continue;
            }
            if (temporary_name(name)) {
                temporaries.push_back(name);
                continue;
            }
            if (state->tombstones.size() >= max_tombstones || name.size() != 41 ||
                name.substr(36) != ".json") {
                return Status(StatusCode::DataLoss, {});
            }
            auto bytes = persistence::read_file(directory, name, kMaxBytes);
            if (!bytes.ok()) {
                return bytes.status();
            }
            auto decoded = decode(bytes.value());
            if (!decoded.ok()) {
                return decoded.status();
            }
            if (file_name(decoded.value().task_id) != name) {
                return Status(StatusCode::DataLoss, {});
            }
            state->tombstones.emplace(decoded.value().task_id, std::move(decoded.value()));
        }
        for (const auto& tmp : temporaries) {
            (void)::unlinkat(dir_fd.get(), tmp.c_str(), 0);
        }
        auto synced = persistence::sync_directory(directory, state->faults);
        if (!synced.ok()) {
            return synced.status();
        }
        return std::unique_ptr<TaskRetirementStore>(new TaskRetirementStore(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<void> TaskRetirementStore::publish(const RetirementTombstone& tombstone) {
    auto encoded = encode(tombstone);
    if (!encoded.ok()) {
        return encoded.status();
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto existing = state_->tombstones.find(tombstone.task_id);
    if (existing != state_->tombstones.end()) {
        if (!(existing->second.operation_id == tombstone.operation_id) ||
            existing->second.request_key != tombstone.request_key ||
            existing->second.request_digest != tombstone.request_digest ||
            existing->second.final_control_generation != tombstone.final_control_generation) {
            return Status(StatusCode::Conflict, {});
        }
        if (existing->second.cleanup_completed && !tombstone.cleanup_completed) {
            return Status(StatusCode::Conflict, {});
        }
        if (existing->second.cleanup_completed == tombstone.cleanup_completed) {
            return {};
        }
    } else if (state_->tombstones.size() >= state_->max_tombstones) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    auto published = persistence::publish_file(
        state_->directory, file_name(tombstone.task_id), encoded.value(),
        existing != state_->tombstones.end() ? persistence::FilePublishMode::Replace
                                             : persistence::FilePublishMode::Create,
        kMaxBytes, state_->faults);
    if (!published.status.ok() || published.disposition != persistence::FileDisposition::Durable) {
        return published.status.ok() ? Status(StatusCode::Unavailable, {}) : published.status;
    }
    state_->tombstones[tombstone.task_id] = tombstone;
    return {};
}

Result<void> TaskRetirementStore::mark_cleanup_completed(TaskId task_id) {
    RetirementTombstone updated;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto it = state_->tombstones.find(task_id);
        if (it == state_->tombstones.end()) {
            return Status(StatusCode::NotFound, {});
        }
        if (it->second.cleanup_completed) {
            return {};
        }
        updated = it->second;
        updated.cleanup_completed = true;
    }
    return publish(updated);
}

Result<void> TaskRetirementStore::collect(TaskId task_id) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto it = state_->tombstones.find(task_id);
    if (it == state_->tombstones.end()) {
        return Status(StatusCode::NotFound, {});
    }
    if (!it->second.cleanup_completed) {
        return Status(StatusCode::InvalidState, {});
    }
    FileDescriptor dir_fd(
        ::open(state_->directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (dir_fd.get() < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    if (state_->faults && state_->faults(persistence::FileStep::RemoveAuthority) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    const auto name = file_name(task_id);
    if (::unlinkat(dir_fd.get(), name.c_str(), 0) != 0 && errno != ENOENT) {
        return Status(StatusCode::Unavailable, {});
    }
    auto synced = persistence::sync_directory(state_->directory, state_->faults);
    if (!synced.ok()) {
        return synced.status();
    }
    state_->tombstones.erase(it);
    return {};
}

bool TaskRetirementStore::is_retired(TaskId task_id) const noexcept {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->tombstones.find(task_id) != state_->tombstones.end();
}

std::optional<RetirementTombstone> TaskRetirementStore::find(TaskId task_id) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto it = state_->tombstones.find(task_id);
    return it == state_->tombstones.end() ? std::optional<RetirementTombstone>{} : it->second;
}

std::vector<RetirementTombstone> TaskRetirementStore::all() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<RetirementTombstone> result;
    result.reserve(state_->tombstones.size());
    for (const auto& [id, tombstone] : state_->tombstones) {
        result.push_back(tombstone);
    }
    return result;
}

std::size_t TaskRetirementStore::count() const noexcept {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->tombstones.size();
}

}  // namespace rodaq::task
