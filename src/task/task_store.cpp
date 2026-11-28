#include "task_store.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <rodaq/logging.hpp>

#include "../persistence/decimal.hpp"
#include "../persistence/file_descriptor.hpp"
#include "../persistence/json.hpp"
#include "../uuid.hpp"
#include "config.hpp"
#include "identity.hpp"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

namespace rodaq::task {
namespace {

using persistence::FileDescriptor;
using persistence::Json;

constexpr std::size_t kMaxMetaBytes = 256 * 1024;
constexpr std::uint32_t kMaxCleanupDepth = 4;
constexpr std::uint32_t kMaxCleanupEntries = 1024;

int rename_noreplace(int dir_fd, const char* from, const char* to) noexcept {
    return static_cast<int>(syscall(SYS_renameat2, dir_fd, from, dir_fd, to, RENAME_NOREPLACE));
}

bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

Result<void> remove_tree_bounded(
    int parent_fd, const char* name, std::uint32_t depth, std::uint32_t& remaining) {
    if (remaining == 0) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    --remaining;
    struct stat info {};
    if (fstatat(parent_fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? Result<void>{} : Result<void>(Status(StatusCode::Unavailable, {}));
    }
    if (S_ISREG(info.st_mode) || S_ISLNK(info.st_mode)) {
        return unlinkat(parent_fd, name, 0) == 0 || errno == ENOENT
                   ? Result<void>{}
                   : Result<void>(Status(StatusCode::Unavailable, {}));
    }
    if (!S_ISDIR(info.st_mode) || depth == 0) {
        return Status(StatusCode::DataLoss, {});
    }
    FileDescriptor opened(openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (opened.get() < 0) {
        return errno == ENOENT ? Result<void>{} : Result<void>(Status(StatusCode::Unavailable, {}));
    }
    const auto close_dir = [](DIR* d) noexcept { (void)closedir(d); };
    std::unique_ptr<DIR, decltype(close_dir)> entries(fdopendir(opened.get()), close_dir);
    if (!entries) {
        return Status(StatusCode::Unavailable, {});
    }
    (void)opened.release();
    const int child_fd = dirfd(entries.get());
    std::vector<std::string> children;
    errno = 0;
    while (const auto* item = readdir(entries.get())) {
        const std::string_view child(item->d_name);
        if (child == "." || child == "..") {
            continue;
        }
        children.emplace_back(child);
        if (children.size() > remaining) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        errno = 0;
    }
    if (errno != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    for (const auto& child : children) {
        auto removed = remove_tree_bounded(child_fd, child.c_str(), depth - 1, remaining);
        if (!removed.ok()) {
            return removed.status();
        }
    }
    entries.reset();
    if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0 && errno != ENOENT) {
        return Status(StatusCode::Unavailable, {});
    }
    return {};
}

Result<std::string> encode_metadata(const TaskMetadata& meta) {
    try {
        Json known = Json::object();
        for (const auto& [stream_id, key] : meta.known_streams) {
            known[std::to_string(stream_id)] = key;
        }
        Json doc{
            {"format_version", "1.0"},
            {"task_id", to_string(meta.task_id)},
            {"active_config_version", meta.active_config_version},
            {"next_pipeline_id", std::to_string(meta.next_pipeline_id)},
            {"next_stream_id", std::to_string(meta.next_stream_id)},
            {"known_streams", std::move(known)}};
        auto dumped = doc.dump();
        if (dumped.size() > kMaxMetaBytes) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        return dumped;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<TaskMetadata> decode_metadata(std::string_view encoded, TaskId expected_task_id) {
    auto parsed = persistence::parse_json(encoded, {kMaxMetaBytes, 8});
    if (!parsed.ok()) {
        return Status(StatusCode::DataLoss, {});
    }
    const auto& doc = parsed.value();
    if (!doc.is_object() || doc.size() != 6 || !doc.contains("format_version") ||
        !doc.at("format_version").is_string() || doc.at("format_version") != "1.0" ||
        !doc.contains("task_id") || !doc.at("task_id").is_string() ||
        !doc.contains("active_config_version") || !doc.at("active_config_version").is_string() ||
        !doc.contains("next_pipeline_id") || !doc.at("next_pipeline_id").is_string() ||
        !doc.contains("next_stream_id") || !doc.at("next_stream_id").is_string() ||
        !doc.contains("known_streams") || !doc.at("known_streams").is_object()) {
        return Status(StatusCode::DataLoss, {});
    }
    auto task_id = parse_task_id(doc.at("task_id").get_ref<const std::string&>());
    if (!task_id.ok() || !(task_id.value() == expected_task_id)) {
        return Status(StatusCode::DataLoss, {});
    }
    const auto& active = doc.at("active_config_version").get_ref<const std::string&>();
    if (!uuid::parse(active).ok()) {
        return Status(StatusCode::DataLoss, {});
    }
    auto next_pipe =
        persistence::unsigned_decimal(doc.at("next_pipeline_id").get_ref<const std::string&>());
    auto next_stream =
        persistence::unsigned_decimal(doc.at("next_stream_id").get_ref<const std::string&>());
    if (!next_pipe.ok() || !next_stream.ok() || next_pipe.value() == 0 ||
        next_stream.value() == 0) {
        return Status(StatusCode::DataLoss, {});
    }
    TaskMetadata meta;
    meta.task_id = task_id.value();
    meta.active_config_version = active;
    meta.next_pipeline_id = next_pipe.value();
    meta.next_stream_id = next_stream.value();
    for (const auto& [k, v] : doc.at("known_streams").items()) {
        auto sid = persistence::unsigned_decimal(k);
        if (!sid.ok() || sid.value() == 0 || !v.is_string() ||
            v.get_ref<const std::string&>().empty()) {
            return Status(StatusCode::DataLoss, {});
        }
        meta.known_streams[sid.value()] = v.get<std::string>();
    }
    return meta;
}

Result<TaskConfig> materialize_ids(
    const TaskConfig& input, TaskMetadata& meta, const TaskConfigLimits& limits, bool is_create) {
    auto valid = validate_task_config(input, limits, false);
    if (!valid.ok()) {
        return valid.status();
    }
    TaskConfig out = input;
    std::set<PipelineId> used_pipelines;
    std::set<StreamId> used_streams;
    for (auto& pipe : out.pipelines) {
        if (pipe.pipeline_id == 0) {
            if (meta.next_pipeline_id == std::numeric_limits<std::uint64_t>::max()) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            pipe.pipeline_id = meta.next_pipeline_id++;
        } else {
            if (is_create) {
                return Status(StatusCode::InvalidArgument, "Create must not supply pipeline_id");
            }
            if (pipe.pipeline_id >= meta.next_pipeline_id) {
                return Status(StatusCode::InvalidArgument, "Unknown pipeline_id");
            }
        }
        if (!used_pipelines.insert(pipe.pipeline_id).second) {
            return Status(StatusCode::InvalidArgument, "Duplicate pipeline_id");
        }
        for (auto& stream : pipe.streams) {
            if (stream.stream_id == 0) {
                if (meta.next_stream_id == std::numeric_limits<std::uint64_t>::max()) {
                    return Status(StatusCode::ResourceExhausted, {});
                }
                stream.stream_id = meta.next_stream_id++;
                meta.known_streams[stream.stream_id] = stream.stream_key;
            } else {
                if (is_create) {
                    return Status(StatusCode::InvalidArgument, "Create must not supply stream_id");
                }
                const auto it = meta.known_streams.find(stream.stream_id);
                if (it == meta.known_streams.end()) {
                    return Status(StatusCode::InvalidArgument, "Unknown stream_id");
                }
                if (it->second != stream.stream_key) {
                    return Status(
                        StatusCode::InvalidArgument,
                        "Cannot rebind existing stream_id to a different stream_key");
                }
            }
            if (!used_streams.insert(stream.stream_id).second) {
                return Status(StatusCode::InvalidArgument, "Duplicate stream_id");
            }
        }
    }
    auto verified = validate_task_config(out, limits, true);
    if (!verified.ok()) {
        return verified.status();
    }
    return out;
}

}  // namespace

TaskConfigStore::TaskConfigStore(
    std::filesystem::path root, TaskStorePolicy policy, persistence::FileFaultHook faults) noexcept
: root_(std::move(root)), policy_(policy), faults_(std::move(faults)) {}

Result<TaskConfigStore> TaskConfigStore::open(
    const std::filesystem::path& tasks_root, TaskStorePolicy policy,
    persistence::FileFaultHook faults) {
    if (tasks_root.empty() || policy.max_config_versions == 0 ||
        policy.max_config_versions > 4096) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(tasks_root, ec);
    if (ec && status.type() != std::filesystem::file_type::not_found) {
        return Status(StatusCode::Unavailable, {});
    }
    if (status.type() == std::filesystem::file_type::not_found) {
        std::filesystem::create_directories(tasks_root, ec);
        if (ec) {
            return Status(StatusCode::Unavailable, {});
        }
        auto synced = persistence::sync_directory(tasks_root.parent_path(), faults);
        if (!synced.ok()) {
            return synced.status();
        }
    } else if (status.type() != std::filesystem::file_type::directory) {
        return Status(StatusCode::DataLoss, {});
    }
    return TaskConfigStore(tasks_root, policy, std::move(faults));
}

Result<std::vector<LoadedTask>> TaskConfigStore::load_all() {
    try {
        FileDescriptor parent(
            ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (parent.get() < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        FileDescriptor dup_fd(openat(parent.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (dup_fd.get() < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        const auto close_dir = [](DIR* d) noexcept { (void)closedir(d); };
        std::unique_ptr<DIR, decltype(close_dir)> entries(fdopendir(dup_fd.get()), close_dir);
        if (!entries) {
            return Status(StatusCode::Unavailable, {});
        }
        (void)dup_fd.release();
        std::vector<std::string> names;
        errno = 0;
        while (const auto* item = readdir(entries.get())) {
            const std::string_view name(item->d_name);
            if (name == "." || name == "..") {
                continue;
            }
            names.emplace_back(name);
            errno = 0;
        }
        if (errno != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        entries.reset();
        std::sort(names.begin(), names.end());

        std::vector<LoadedTask> loaded;
        bool modified = false;
        for (const auto& name : names) {
            if (ends_with(name, ".tmp")) {
                std::uint32_t remaining = kMaxCleanupEntries;
                auto removed =
                    remove_tree_bounded(parent.get(), name.c_str(), kMaxCleanupDepth, remaining);
                if (!removed.ok()) {
                    logging::write(
                        logging::Level::Warning, {},
                        "Startup failed to remove stale task tmp directory: " + name);
                } else {
                    modified = true;
                }
                continue;
            }
            auto task_id = parse_task_id(name);
            if (!task_id.ok()) {
                logging::write(
                    logging::Level::Warning, {}, "Startup ignored non-task entry: " + name);
                continue;
            }
            const auto task_dir = root_ / name;
            auto meta_bytes = persistence::read_file(task_dir, "meta.json", kMaxMetaBytes);
            if (!meta_bytes.ok()) {
                logging::write(
                    logging::Level::Warning, {}, "Startup skipped unreadable task: " + name);
                continue;
            }
            auto meta = decode_metadata(meta_bytes.value(), task_id.value());
            if (!meta.ok()) {
                logging::write(
                    logging::Level::Warning, {}, "Startup skipped corrupt task meta: " + name);
                continue;
            }
            auto cfg_bytes = persistence::read_file(
                task_dir / "configs", meta.value().active_config_version + ".json",
                policy_.config.max_config_bytes);
            if (!cfg_bytes.ok()) {
                logging::write(
                    logging::Level::Warning, {},
                    "Startup skipped task with missing config: " + name);
                continue;
            }
            auto cfg = parse_task_config(cfg_bytes.value(), policy_.config);
            if (!cfg.ok() || !validate_task_config(cfg.value(), policy_.config, true).ok()) {
                logging::write(
                    logging::Level::Warning, {}, "Startup skipped corrupt task config: " + name);
                continue;
            }
            loaded.push_back(LoadedTask{std::move(meta.value()), std::move(cfg.value())});
        }
        if (modified && ::fsync(parent.get()) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        return loaded;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<LoadedTask> TaskConfigStore::create_task(TaskId task_id, const TaskConfig& input) {
    if (!uuid::nonzero(task_id.value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        TaskMetadata meta;
        meta.task_id = task_id;
        auto materialized = materialize_ids(input, meta, policy_.config, true);
        if (!materialized.ok()) {
            return materialized.status();
        }
        auto version = new_config_version();
        if (!version.ok()) {
            return version.status();
        }
        meta.active_config_version = std::move(version.value());
        auto encoded_cfg = export_task_config(materialized.value(), policy_.config);
        if (!encoded_cfg.ok()) {
            return encoded_cfg.status();
        }
        auto encoded_meta = encode_metadata(meta);
        if (!encoded_meta.ok()) {
            return encoded_meta.status();
        }

        const std::string final_name = to_string(task_id);
        const std::string tmp_name = final_name + ".tmp";
        const auto tmp_dir = root_ / tmp_name;
        const auto configs_dir = tmp_dir / "configs";

        std::error_code ec;
        if (!std::filesystem::create_directory(tmp_dir, ec) || ec) {
            return Status(StatusCode::Conflict, {});
        }
        auto cleanup_tmp = [&]() noexcept {
            try {
                FileDescriptor parent(
                    ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (parent.get() >= 0) {
                    std::uint32_t remaining = kMaxCleanupEntries;
                    (void)remove_tree_bounded(
                        parent.get(), tmp_name.c_str(), kMaxCleanupDepth, remaining);
                }
            } catch (...) {
            }
        };
        if (!std::filesystem::create_directory(configs_dir, ec) || ec) {
            cleanup_tmp();
            return Status(StatusCode::Unavailable, {});
        }
        auto published_cfg = persistence::publish_file(
            configs_dir, meta.active_config_version + ".json", encoded_cfg.value(),
            persistence::FilePublishMode::Create, policy_.config.max_config_bytes, faults_);
        if (!published_cfg.status.ok()) {
            cleanup_tmp();
            return published_cfg.status;
        }
        auto published_meta = persistence::publish_file(
            tmp_dir, "meta.json", encoded_meta.value(), persistence::FilePublishMode::Create,
            kMaxMetaBytes, faults_);
        if (!published_meta.status.ok()) {
            cleanup_tmp();
            return published_meta.status;
        }

        FileDescriptor parent(
            ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (parent.get() < 0) {
            cleanup_tmp();
            return Status(StatusCode::Unavailable, {});
        }
        if (rename_noreplace(parent.get(), tmp_name.c_str(), final_name.c_str()) != 0) {
            const int err = errno;
            cleanup_tmp();
            return Status(err == EEXIST ? StatusCode::Conflict : StatusCode::Unavailable, {});
        }
        if (::fsync(parent.get()) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        return LoadedTask{std::move(meta), std::move(materialized.value())};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<LoadedTask> TaskConfigStore::update_config(
    const TaskMetadata& current, const TaskConfig& input) {
    try {
        TaskMetadata next = current;
        auto materialized = materialize_ids(input, next, policy_.config, false);
        if (!materialized.ok()) {
            return materialized.status();
        }
        auto version = new_config_version();
        if (!version.ok()) {
            return version.status();
        }
        next.active_config_version = std::move(version.value());
        auto encoded_cfg = export_task_config(materialized.value(), policy_.config);
        if (!encoded_cfg.ok()) {
            return encoded_cfg.status();
        }
        auto encoded_meta = encode_metadata(next);
        if (!encoded_meta.ok()) {
            return encoded_meta.status();
        }

        const auto task_dir = root_ / to_string(current.task_id);
        const auto configs_dir = task_dir / "configs";
        auto published_cfg = persistence::publish_file(
            configs_dir, next.active_config_version + ".json", encoded_cfg.value(),
            persistence::FilePublishMode::Create, policy_.config.max_config_bytes, faults_);
        if (!published_cfg.status.ok()) {
            return published_cfg.status;
        }
        auto published_meta = persistence::publish_file(
            task_dir, "meta.json", encoded_meta.value(), persistence::FilePublishMode::Replace,
            kMaxMetaBytes, faults_);
        if (!published_meta.status.ok()) {
            return published_meta.status;
        }
        return LoadedTask{std::move(next), std::move(materialized.value())};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> TaskConfigStore::remove_task(TaskId task_id) {
    try {
        FileDescriptor parent(
            ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (parent.get() < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        const std::string final_name = to_string(task_id);
        const std::string tmp_name = final_name + ".tmp";
        if (rename_noreplace(parent.get(), final_name.c_str(), tmp_name.c_str()) != 0) {
            if (errno == ENOENT) {
                return {};
            }
            return Status(StatusCode::Unavailable, {});
        }
        std::uint32_t remaining = kMaxCleanupEntries;
        auto removed =
            remove_tree_bounded(parent.get(), tmp_name.c_str(), kMaxCleanupDepth, remaining);
        if (!removed.ok()) {
            return removed.status();
        }
        if (::fsync(parent.get()) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::task
