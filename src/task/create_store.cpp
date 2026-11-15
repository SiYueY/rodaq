#include "create_store.hpp"

#include "identity.hpp"
#include "../persistence/json.hpp"
#include "../time/wall_time.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rodaq::task {
namespace {

constexpr std::string_view kDomain = "rodaq.create-receipt.v1";
constexpr std::size_t kMaxBytes = 2048;

persistence::JsonLimits limits() { return {kMaxBytes, 2, 128, 8, 0, 16}; }

struct FileDescriptor {
    int value{-1};
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    explicit FileDescriptor(int descriptor) noexcept : value(descriptor) {}
    ~FileDescriptor() noexcept {
        if (value >= 0) {
            close(value);
        }
    }
};

struct StoredReceipt {
    CreateReceipt receipt;
    std::string content_digest;
};

bool valid_receipt(const CreateReceipt& receipt) noexcept {
    return persistence::valid_digest(receipt.request_key_hash) &&
           persistence::valid_digest(receipt.request_digest) &&
           std::any_of(
               receipt.assigned_task_id.value.begin(), receipt.assigned_task_id.value.end(),
               [](std::uint8_t byte) { return byte != 0; }) &&
           (receipt.phase == CreatePhase::Allocated ||
            receipt.phase == CreatePhase::Materializing || receipt.phase == CreatePhase::Completed);
}

Result<std::string> encode(const CreateReceipt& receipt) {
    if (!valid_receipt(receipt)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto timestamp = time::format_wall_time(receipt.created_at);
    if (!timestamp.ok()) {
        return std::move(timestamp).status();
    }
    const char* phase = receipt.phase == CreatePhase::Allocated       ? "Allocated"
                        : receipt.phase == CreatePhase::Materializing ? "Materializing"
                                                                      : "Completed";
    const persistence::Json document{
        {"format_version", "1.0"},
        {"request_key_hash", receipt.request_key_hash},
        {"request_digest", receipt.request_digest},
        {"assigned_task_id", to_string(receipt.assigned_task_id)},
        {"phase", phase},
        {"created_at", std::move(timestamp).value()}};
    return persistence::encode_integrity(document, kDomain, limits());
}

Result<StoredReceipt> decode(std::string_view bytes) {
    auto parsed = persistence::parse_json(bytes, limits());
    if (!parsed.ok()) {
        return parsed.status().code() == StatusCode::InvalidArgument
                   ? Status(StatusCode::DataLoss, {})
                   : std::move(parsed).status();
    }
    const auto& document = parsed.value();
    if (!document.is_object() || document.size() != 7) {
        return Status(StatusCode::DataLoss, "Invalid Create receipt structure");
    }
    for (const auto* field :
         {"format_version", "request_key_hash", "request_digest", "assigned_task_id", "phase",
          "created_at", "content_digest"}) {
        auto found = document.find(field);
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
    auto task_id = parse_task_id(text("assigned_task_id"));
    auto timestamp = time::parse_wall_time(text("created_at"));
    if (!timestamp.ok() && timestamp.status().code() == StatusCode::ResourceExhausted) {
        return std::move(timestamp).status();
    }
    if (!task_id.ok() || !timestamp.ok()) {
        return Status(StatusCode::DataLoss, "Invalid Create receipt identity or timestamp");
    }
    CreatePhase phase;
    if (text("phase") == "Allocated") {
        phase = CreatePhase::Allocated;
    } else if (text("phase") == "Materializing") {
        phase = CreatePhase::Materializing;
    } else if (text("phase") == "Completed") {
        phase = CreatePhase::Completed;
    } else {
        return Status(StatusCode::DataLoss, "Unknown Create receipt phase");
    }
    CreateReceipt receipt{
        text("request_key_hash"), text("request_digest"), task_id.value(), phase,
        timestamp.value()};
    if (!valid_receipt(receipt)) {
        return Status(StatusCode::DataLoss, {});
    }
    auto verified = persistence::verify_integrity(document, kDomain, limits());
    if (!verified.ok()) {
        return std::move(verified).status();
    }
    return StoredReceipt{std::move(receipt), text("content_digest")};
}

std::string file_name(std::string_view key_hash) {
    return std::string(key_hash.substr(7)) + ".json";
}

bool temporary_name(std::string_view name) noexcept {
    return name.size() == 43 && name.substr(0, 11) == ".rodaq-tmp-" &&
           std::all_of(name.begin() + 11, name.end(), [](char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

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
    int result;
    do {
        result = fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result == 0 ? Result<void>{} : Result<void>{Status(StatusCode::Unavailable, {})};
}

}  // namespace

struct TaskCreateStore::State {
    std::filesystem::path directory;
    std::size_t max_receipts{0};
    persistence::FileFaultHook faults;
    std::mutex execution;
    int lease{-1};
    bool protected_state{true};
    std::map<std::string, StoredReceipt, std::less<>> receipts;

    ~State() noexcept {
        if (lease >= 0) {
            close(lease);
        }
    }

    Result<void> check_actual(const std::string& key, const StoredReceipt& expected) {
        auto bytes = persistence::read_file(directory, file_name(key), kMaxBytes);
        if (!bytes.ok()) {
            protected_state = true;
            return std::move(bytes).status();
        }
        auto actual = decode(bytes.value());
        if (!actual.ok()) {
            protected_state = true;
            return std::move(actual).status();
        }
        if (actual.value().content_digest != expected.content_digest) {
            protected_state = true;
            return Status(StatusCode::Conflict, {});
        }
        return {};
    }

    Result<void> reload() {
        protected_state = true;
        std::map<std::string, StoredReceipt, std::less<>> loaded;
        std::set<TaskId> task_ids;
        std::vector<std::string> temporaries;
        std::size_t entries = 0;
        FileDescriptor directory_fd(
            ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (directory_fd.value < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (++entries > max_receipts + 65) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            const auto name = entry.path().filename().string();
            if (name == ".create.lock") {
                continue;
            }
            if (temporary_name(name)) {
                if (temporaries.size() >= 64) {
                    return Status(StatusCode::ResourceExhausted, {});
                }
                temporaries.push_back(name);
                continue;
            }
            if (loaded.size() >= max_receipts || name.size() != 69 || name.substr(64) != ".json") {
                return Status(StatusCode::DataLoss, "Unknown or excessive Create namespace entry");
            }
            auto bytes = persistence::read_file(directory, name, kMaxBytes);
            if (!bytes.ok()) {
                return std::move(bytes).status();
            }
            auto decoded = decode(bytes.value());
            if (!decoded.ok()) {
                return std::move(decoded).status();
            }
            auto& receipt = decoded.value().receipt;
            if (file_name(receipt.request_key_hash) != name ||
                !task_ids.insert(receipt.assigned_task_id).second) {
                return Status(
                    StatusCode::DataLoss, "Create filename mismatch or duplicate Task identity");
            }
            // A successfully read file may be the result of a previous uncertain
            // rename. Sync the actual file and parent before trusting that fact.
            FileDescriptor record_fd(openat(
                directory_fd.value, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            struct stat record_stat {};
            if (record_fd.value < 0 || fstat(record_fd.value, &record_stat) != 0 ||
                !S_ISREG(record_stat.st_mode)) {
                return Status(StatusCode::DataLoss, "Create authority file is not a regular file");
            }
            auto synced = sync_descriptor(record_fd.value, persistence::FileStep::SyncFile, faults);
            if (!synced.ok()) {
                return std::move(synced).status();
            }
            const auto key = receipt.request_key_hash;
            loaded.emplace(key, std::move(decoded).value());
        }
        for (const auto& name : temporaries) {
            if (injected(faults, persistence::FileStep::RemoveTemporary) != 0 ||
                unlinkat(directory_fd.value, name.c_str(), 0) != 0) {
                return Status(StatusCode::Unavailable, {});
            }
        }
        auto synced =
            sync_descriptor(directory_fd.value, persistence::FileStep::SyncDirectory, faults);
        if (!synced.ok()) {
            return std::move(synced).status();
        }
        receipts.swap(loaded);
        protected_state = false;
        return {};
    }
};

TaskCreateStore::TaskCreateStore(std::unique_ptr<State> state) noexcept
: state_(std::move(state)) {}

TaskCreateStore::~TaskCreateStore() noexcept = default;

Result<std::unique_ptr<TaskCreateStore>> TaskCreateStore::open(
    const std::filesystem::path& directory, std::size_t max_receipts,
    persistence::FileFaultHook faults) {
    if (max_receipts == 0 || max_receipts > 65536) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto state = std::make_unique<State>();
        state->directory = directory;
        state->max_receipts = max_receipts;
        state->faults = std::move(faults);
        state->lease = ::open(
            (directory / ".create.lock").c_str(),
            O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        struct stat lock_stat {};
        if (state->lease < 0 || fstat(state->lease, &lock_stat) != 0 ||
            !S_ISREG(lock_stat.st_mode)) {
            return Status(StatusCode::Unavailable, {});
        }
        if (flock(state->lease, LOCK_EX | LOCK_NB) != 0) {
            return Status(
                errno == EWOULDBLOCK ? StatusCode::Conflict : StatusCode::Unavailable, {});
        }
        auto loaded = state->reload();
        if (!loaded.ok()) {
            return std::move(loaded).status();
        }
        return std::unique_ptr<TaskCreateStore>(new TaskCreateStore(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<CreateClaim> TaskCreateStore::claim(
    std::string_view key_hash, std::string_view request_digest, TaskId candidate_task_id,
    WallTime created_at) {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    try {
        CreateReceipt receipt{
            std::string(key_hash), std::string(request_digest), candidate_task_id,
            CreatePhase::Allocated, created_at};
        if (!valid_receipt(receipt)) {
            return Status(StatusCode::InvalidArgument, {});
        }
        auto found = state_->receipts.find(key_hash);
        if (found != state_->receipts.end()) {
            if (found->second.receipt.request_digest != request_digest) {
                return Status(StatusCode::Conflict, {});
            }
            auto actual = state_->check_actual(found->first, found->second);
            if (!actual.ok()) {
                return std::move(actual).status();
            }
            return CreateClaim{
                found->second.receipt.phase == CreatePhase::Completed
                    ? CreateClaimState::ExistingCompleted
                    : CreateClaimState::ExistingInProgress,
                found->second.receipt};
        }
        if (state_->receipts.size() >= state_->max_receipts) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        for (const auto& item : state_->receipts) {
            if (item.second.receipt.assigned_task_id == candidate_task_id) {
                return Status(StatusCode::Conflict, {});
            }
        }
        auto bytes = encode(receipt);
        if (!bytes.ok()) {
            return std::move(bytes).status();
        }
        auto stored = decode(bytes.value());
        if (!stored.ok()) {
            return std::move(stored).status();
        }
        // Allocate both the response and cache node before native publication.
        CreateClaim response{CreateClaimState::Claimed, receipt};
        std::map<std::string, StoredReceipt, std::less<>> reserved;
        reserved.emplace(receipt.request_key_hash, std::move(stored).value());
        auto node = reserved.extract(reserved.begin());
        auto published = persistence::publish_file(
            state_->directory, file_name(key_hash), bytes.value(),
            persistence::FilePublishMode::Create, kMaxBytes, state_->faults);
        if (published.disposition != persistence::FileDisposition::Durable) {
            state_->protected_state =
                published.disposition == persistence::FileDisposition::Uncertain ||
                !published.retained_temporary.empty() ||
                published.status.code() == StatusCode::Conflict;
            return std::move(published.status);
        }
        state_->receipts.insert(std::move(node));
        return response;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<void> TaskCreateStore::publish(const CreateReceipt& receipt) {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    try {
        if (!valid_receipt(receipt)) {
            return Status(StatusCode::InvalidArgument, {});
        }
        const auto found = state_->receipts.find(receipt.request_key_hash);
        if (found == state_->receipts.end()) {
            return Status(StatusCode::NotFound, {});
        }
        const auto& previous = found->second.receipt;
        if (previous.request_digest != receipt.request_digest ||
            !(previous.assigned_task_id == receipt.assigned_task_id) ||
            previous.created_at != receipt.created_at ||
            static_cast<int>(receipt.phase) < static_cast<int>(previous.phase) ||
            static_cast<int>(receipt.phase) > static_cast<int>(previous.phase) + 1) {
            return Status(StatusCode::Conflict, {});
        }
        const auto name = file_name(receipt.request_key_hash);
        auto actual = state_->check_actual(found->first, found->second);
        if (!actual.ok()) {
            return std::move(actual).status();
        }
        if (receipt.phase == previous.phase) {
            return {};
        }
        auto bytes = encode(receipt);
        if (!bytes.ok()) {
            return std::move(bytes).status();
        }
        auto updated = decode(bytes.value());
        if (!updated.ok()) {
            return std::move(updated).status();
        }
        auto published = persistence::publish_file(
            state_->directory, name, bytes.value(), persistence::FilePublishMode::Replace,
            kMaxBytes, state_->faults);
        if (published.disposition != persistence::FileDisposition::Durable) {
            state_->protected_state =
                published.disposition == persistence::FileDisposition::Uncertain ||
                !published.retained_temporary.empty();
            return std::move(published.status);
        }
        found->second = std::move(updated).value();
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<void> TaskCreateStore::reconcile() {
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

Result<std::optional<CreateReceipt>> TaskCreateStore::lookup(
    std::string_view key_hash, std::string_view digest) {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    if (!persistence::valid_digest(key_hash) || !persistence::valid_digest(digest)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        const auto found = state_->receipts.find(key_hash);
        if (found == state_->receipts.end()) {
            return std::optional<CreateReceipt>{};
        }
        if (found->second.receipt.request_digest != digest) {
            return Status(StatusCode::Conflict, {});
        }
        auto actual = state_->check_actual(found->first, found->second);
        if (!actual.ok()) {
            return actual.status();
        }
        return std::optional<CreateReceipt>{found->second.receipt};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::vector<CreateReceipt>> TaskCreateStore::retained_receipts() {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    try {
        std::vector<CreateReceipt> receipts;
        receipts.reserve(state_->receipts.size());
        for (const auto& entry : state_->receipts) {
            receipts.push_back(entry.second.receipt);
        }
        return receipts;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::task
