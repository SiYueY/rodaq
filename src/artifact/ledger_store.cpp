#include "ledger_store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <sys/file.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

#include "../persistence/decimal.hpp"
#include "../persistence/file_descriptor.hpp"

namespace rodaq::artifact {
namespace {

using persistence::FileDescriptor;
static_assert(std::is_nothrow_move_constructible_v<LedgerRecord>);
static_assert(std::is_nothrow_move_assignable_v<LedgerRecord>);
enum class SlotPhase { Empty, Reserved, Occupied, Uncertain, Collecting, Aborting };

struct LedgerSlot {
    std::uint64_t identity{0};
    std::uint64_t generation{0};
    SlotPhase phase{SlotPhase::Empty};
    bool token_present{false};
    std::optional<LedgerRecord> current;
    std::optional<LedgerRecord> pending;
    std::string retained_temporary;
};

std::string shard_name(std::uint64_t identity) {
    constexpr char kHex[] = "0123456789abcdef";
    return {kHex[(identity >> 4u) & 15u], kHex[identity & 15u]};
}

std::string file_name(std::uint64_t identity) { return std::to_string(identity) + ".json"; }

bool same_directory(const struct stat& expected, const struct stat& actual) noexcept {
    return S_ISDIR(actual.st_mode) && expected.st_dev == actual.st_dev &&
           expected.st_ino == actual.st_ino;
}

Result<bool> same_record(const LedgerRecord& left, const LedgerRecord& right) {
    auto old_bytes = encode_ledger_record(left);
    if (!old_bytes.ok()) {
        return old_bytes.status();
    }
    auto new_bytes = encode_ledger_record(right);
    if (!new_bytes.ok()) {
        return new_bytes.status();
    }
    return old_bytes.value() == new_bytes.value();
}

Result<LedgerRecord> read_record(const std::filesystem::path& shard, std::uint64_t identity) {
    struct stat attributes {};
    if (lstat((shard / file_name(identity)).c_str(), &attributes) != 0) {
        return Status(errno == ENOENT ? StatusCode::NotFound : StatusCode::Unavailable, {});
    }
    if (!S_ISREG(attributes.st_mode) || attributes.st_nlink != 1) {
        return Status(StatusCode::DataLoss, {});
    }
    auto bytes = persistence::read_file(shard, file_name(identity), 4096);
    if (!bytes.ok()) {
        return bytes.status();
    }
    auto record = decode_ledger_record(bytes.value());
    if (!record.ok()) {
        return record.status();
    }
    if (record.value().segment_id != identity) {
        return Status(StatusCode::DataLoss, {});
    }
    return record;
}

}  // namespace

struct LedgerStoreState {
    std::filesystem::path configured_directory;
    std::filesystem::path directory;
    FileDescriptor root;
    FileDescriptor lease;
    struct stat root_identity {};
    struct stat lease_identity {};
    std::array<struct stat, 256> shards{};
    std::uint32_t max_records{0};
    std::size_t slots_in_use{0};
    std::vector<LedgerSlot> slots;
    persistence::FileFaultHook faults;
    // The actual durable flight owns execution; capacity/cache never waits for
    // that flight. No structure mutex is held during filesystem operations.
    std::mutex execution;
    mutable std::mutex projection;
    std::atomic<StatusCode> protection{StatusCode::Ok};

    LedgerStoreState(FileDescriptor root_fd, FileDescriptor lease_fd)
    : root(std::move(root_fd)), lease(std::move(lease_fd)) {}

    std::size_t find(std::uint64_t identity) const noexcept {
        for (std::size_t index = 0; index < slots.size(); ++index) {
            if (slots[index].phase != SlotPhase::Empty && slots[index].identity == identity) {
                return index;
            }
        }
        return slots.size();
    }

    void withhold(std::size_t index) {
        std::lock_guard<std::mutex> lock(projection);
        slots[index].phase = SlotPhase::Uncertain;
    }

    Result<void> verify_namespace(std::uint64_t identity) {
        if (protection.load() != StatusCode::Ok) {
            return Status(protection.load(), {});
        }
        struct stat actual {};
        if (std::filesystem::canonical(configured_directory) != directory ||
            lstat(directory.c_str(), &actual) != 0 || !same_directory(root_identity, actual) ||
            fstatat(root.get(), ".ledger.lock", &actual, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(actual.st_mode) || actual.st_nlink != 1 ||
            actual.st_dev != lease_identity.st_dev || actual.st_ino != lease_identity.st_ino ||
            fstatat(root.get(), shard_name(identity).c_str(), &actual, AT_SYMLINK_NOFOLLOW) != 0 ||
            !same_directory(shards[identity & 255u], actual)) {
            protection.store(StatusCode::DataLoss);
            return Status(StatusCode::DataLoss, {});
        }
        return {};
    }
};

LedgerRegistration::LedgerRegistration(
    std::shared_ptr<LedgerStoreState> state, std::size_t slot, std::uint64_t generation,
    std::uint64_t segment_id) noexcept
: state_(std::move(state)), slot_(slot), generation_(generation), segment_id_(segment_id) {}

LedgerRegistration::LedgerRegistration(LedgerRegistration&& other) noexcept
: state_(std::move(other.state_)),
  slot_(other.slot_),
  generation_(other.generation_),
  segment_id_(other.segment_id_) {}

LedgerRegistration::~LedgerRegistration() noexcept {
    if (!state_) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_->projection);
    auto& slot = state_->slots[slot_];
    if (slot.generation != generation_) {
        return;
    }
    slot.token_present = false;
    if (slot.phase == SlotPhase::Reserved) {
        slot.phase = SlotPhase::Empty;
        slot.identity = 0;
        --state_->slots_in_use;
    }
    // Uncertain and occupied facts are retained, never credited by timeout or
    // loss of the external registration wrapper.
}

LedgerStore::LedgerStore(std::shared_ptr<LedgerStoreState> state) noexcept
: state_(std::move(state)) {}

LedgerStore::~LedgerStore() noexcept = default;

Result<std::unique_ptr<LedgerStore>> LedgerStore::open(
    const std::filesystem::path& directory, LedgerOpenMode mode, std::uint32_t max_records,
    persistence::FileFaultHook faults) {
    if ((mode != LedgerOpenMode::Initialize && mode != LedgerOpenMode::Existing) ||
        max_records == 0 || max_records > 65536) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        const auto canonical = std::filesystem::canonical(directory);
        FileDescriptor root(
            ::open(canonical.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (root.get() < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        FileDescriptor lease(openat(
            root.get(), ".ledger.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC,
            0600));
        struct stat attributes {};
        if (lease.get() < 0 || fstat(lease.get(), &attributes) != 0 ||
            !S_ISREG(attributes.st_mode) || attributes.st_nlink != 1) {
            return Status(StatusCode::DataLoss, {});
        }
        if (flock(lease.get(), LOCK_EX | LOCK_NB) != 0) {
            return Status(
                errno == EWOULDBLOCK ? StatusCode::Conflict : StatusCode::Unavailable, {});
        }
        auto state = std::make_shared<LedgerStoreState>(std::move(root), std::move(lease));
        state->directory = canonical;
        state->configured_directory = std::filesystem::absolute(directory);
        state->max_records = max_records;
        state->faults = std::move(faults);
        state->lease_identity = attributes;
        if (fstat(state->root.get(), &state->root_identity) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        for (unsigned index = 0; index < 256; ++index) {
            const auto name = shard_name(index);
            if (mode == LedgerOpenMode::Initialize &&
                mkdirat(state->root.get(), name.c_str(), 0700) != 0 && errno != EEXIST) {
                return Status(StatusCode::Unavailable, {});
            }
            FileDescriptor shard(openat(
                state->root.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (shard.get() < 0 || fstat(shard.get(), &state->shards[index]) != 0) {
                return Status(StatusCode::DataLoss, {});
            }
            if (mode == LedgerOpenMode::Initialize) {
                auto synced = persistence::sync_directory(canonical / name, state->faults);
                if (!synced.ok()) {
                    return synced.status();
                }
            }
        }
        auto synced = persistence::sync_directory(canonical, state->faults);
        if (!synced.ok()) {
            return synced.status();
        }
        std::size_t root_entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(canonical)) {
            if (++root_entries > 257) {
                return Status(StatusCode::DataLoss, {});
            }
            const auto name = entry.path().filename().string();
            if (name == ".ledger.lock") {
                continue;
            }
            if (name.size() != 2 ||
                name.find_first_not_of("0123456789abcdef") != std::string::npos ||
                entry.is_symlink() || !entry.is_directory()) {
                return Status(StatusCode::DataLoss, {});
            }
        }
        std::vector<LedgerRecord> recovered;
        recovered.reserve(max_records);
        std::size_t temporary_count = 0;
        for (unsigned shard = 0; shard < 256; ++shard) {
            const auto parent = canonical / shard_name(shard);
            bool changed = false;
            bool has_records = false;
            for (const auto& entry : std::filesystem::directory_iterator(parent)) {
                const auto name = entry.path().filename().string();
                if (persistence::is_temporary_file_name(name)) {
                    if (++temporary_count > 64 || entry.is_symlink() || !entry.is_regular_file()) {
                        return Status(StatusCode::DataLoss, {});
                    }
                    std::filesystem::remove(entry.path());
                    changed = true;
                    continue;
                }
                if (recovered.size() >= 65536 || name.size() < 6 || name.size() > 25 ||
                    name.substr(name.size() - 5) != ".json") {
                    return Status(StatusCode::DataLoss, {});
                }
                auto identity = persistence::unsigned_decimal(
                    std::string_view(name).substr(0, name.size() - 5));
                if (!identity.ok() || identity.value() == 0 || (identity.value() & 255u) != shard) {
                    return Status(StatusCode::DataLoss, {});
                }
                auto record = read_record(parent, identity.value());
                if (!record.ok()) {
                    return record.status();
                }
                synced = persistence::sync_file(parent, name, state->faults);
                if (!synced.ok()) {
                    return synced.status();
                }
                recovered.push_back(std::move(record.value()));
                has_records = true;
            }
            if (changed || has_records) {
                synced = persistence::sync_directory(parent, state->faults);
                if (!synced.ok()) {
                    return synced.status();
                }
            }
        }
        state->slots.resize(std::max<std::size_t>(max_records, recovered.size()));
        for (auto& record : recovered) {
            auto& slot = state->slots[state->slots_in_use++];
            slot.identity = record.segment_id;
            slot.generation = 1;
            slot.phase = SlotPhase::Occupied;
            slot.current.emplace(std::move(record));
        }
        return std::unique_ptr<LedgerStore>(new LedgerStore(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<LedgerRegistration> LedgerStore::reserve_registration(std::uint64_t segment_id) {
    if (segment_id == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (state_->protection.load() != StatusCode::Ok) {
        return Status(state_->protection.load(), {});
    }
    std::lock_guard<std::mutex> lock(state_->projection);
    if (state_->find(segment_id) != state_->slots.size()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->slots_in_use >= state_->max_records) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    for (std::size_t index = 0; index < state_->slots.size(); ++index) {
        auto& slot = state_->slots[index];
        if (slot.phase == SlotPhase::Empty &&
            slot.generation != std::numeric_limits<std::uint64_t>::max()) {
            ++slot.generation;
            slot.identity = segment_id;
            slot.phase = SlotPhase::Reserved;
            slot.token_present = true;
            ++state_->slots_in_use;
            return LedgerRegistration(state_, index, slot.generation, segment_id);
        }
    }
    return Status(StatusCode::ResourceExhausted, {});
}

Result<void> LedgerStore::create_committing(
    LedgerRegistration& registration, const LedgerRecord& record) {
    if (registration.state_ != state_ || record.state != LedgerState::Committing) {
        return Status(StatusCode::Conflict, {});
    }
    auto encoded = encode_ledger_record(record);
    if (!encoded.ok()) {
        return encoded.status();
    }
    std::unique_lock<std::mutex> flight(state_->execution, std::try_to_lock);
    if (!flight.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    try {
        std::optional<LedgerRecord> existing;
        {
            std::lock_guard<std::mutex> lock(state_->projection);
            const auto& slot = state_->slots[registration.slot_];
            if (slot.generation != registration.generation_ || slot.identity != record.segment_id ||
                !slot.token_present) {
                return Status(StatusCode::Conflict, {});
            }
            if (slot.phase == SlotPhase::Uncertain || slot.phase == SlotPhase::Collecting ||
                slot.phase == SlotPhase::Aborting) {
                return Status(StatusCode::Unavailable, {});
            }
            existing = slot.current;
        }
        auto valid = state_->verify_namespace(record.segment_id);
        if (!valid.ok()) {
            return valid.status();
        }
        const auto parent = state_->directory / shard_name(record.segment_id);
        auto actual = read_record(parent, record.segment_id);
        if (existing) {
            auto same = same_record(*existing, record);
            if (!same.ok()) {
                return same.status();
            }
            if (!same.value()) {
                return Status(StatusCode::Conflict, {});
            }
            if (!actual.ok()) {
                state_->withhold(registration.slot_);
                return actual.status().code() == StatusCode::NotFound
                           ? Status(StatusCode::DataLoss, {})
                           : actual.status();
            }
            same = same_record(actual.value(), record);
            if (!same.ok()) {
                return same.status();
            }
            if (!same.value()) {
                state_->withhold(registration.slot_);
                return Status(StatusCode::DataLoss, {});
            }
            std::lock_guard<std::mutex> lock(state_->projection);
            state_->slots[registration.slot_].token_present = false;
        } else {
            if (actual.ok()) {
                state_->withhold(registration.slot_);
                return Status(StatusCode::DataLoss, {});
            }
            if (actual.status().code() != StatusCode::NotFound) {
                state_->withhold(registration.slot_);
                return actual.status();
            }
            auto candidate = record;
            {
                std::lock_guard<std::mutex> lock(state_->projection);
                auto& slot = state_->slots[registration.slot_];
                slot.pending.emplace(std::move(candidate));
                slot.phase = SlotPhase::Uncertain;
            }
            auto published = persistence::publish_file(
                parent, file_name(record.segment_id), encoded.value(),
                persistence::FilePublishMode::Create, 4096, state_->faults);
            {
                std::lock_guard<std::mutex> lock(state_->projection);
                auto& slot = state_->slots[registration.slot_];
                slot.retained_temporary = std::move(published.retained_temporary);
                if (!published.status.ok()) {
                    return std::move(published.status);
                }
                slot.current = std::move(slot.pending);
                slot.pending.reset();
                slot.phase = SlotPhase::Occupied;
                slot.token_present = false;
            }
        }
        registration.state_.reset();
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        state_->protection.store(StatusCode::Unavailable);
        return Status(StatusCode::Unavailable, {});
    }
}

Result<void> LedgerStore::advance(const LedgerRecord& record, LedgerState expected_state) {
    auto encoded = encode_ledger_record(record);
    if (!encoded.ok()) {
        return encoded.status();
    }
    std::unique_lock<std::mutex> flight(state_->execution, std::try_to_lock);
    if (!flight.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    try {
        std::size_t index = 0;
        LedgerRecord previous;
        {
            std::lock_guard<std::mutex> lock(state_->projection);
            index = state_->find(record.segment_id);
            if (index == state_->slots.size()) {
                return Status(StatusCode::NotFound, {});
            }
            const auto& slot = state_->slots[index];
            if (slot.phase != SlotPhase::Occupied || !slot.current) {
                return Status(StatusCode::Unavailable, {});
            }
            if (slot.current->state != expected_state) {
                return Status(StatusCode::Conflict, {});
            }
            previous = *slot.current;
        }
        auto valid = validate_ledger_transition(previous, record);
        if (!valid.ok()) {
            return valid.status();
        }
        valid = state_->verify_namespace(record.segment_id);
        if (!valid.ok()) {
            return valid.status();
        }
        const auto parent = state_->directory / shard_name(record.segment_id);
        auto actual = read_record(parent, record.segment_id);
        if (!actual.ok()) {
            state_->withhold(index);
            return actual.status().code() == StatusCode::NotFound ? Status(StatusCode::DataLoss, {})
                                                                  : actual.status();
        }
        auto same = same_record(actual.value(), previous);
        if (!same.ok()) {
            return same.status();
        }
        if (!same.value()) {
            state_->withhold(index);
            return Status(StatusCode::DataLoss, {});
        }
        if (previous.state == record.state) {
            return {};
        }
        auto candidate = record;
        {
            std::lock_guard<std::mutex> lock(state_->projection);
            state_->slots[index].pending.emplace(std::move(candidate));
            state_->slots[index].phase = SlotPhase::Uncertain;
        }
        auto published = persistence::publish_file(
            parent, file_name(record.segment_id), encoded.value(),
            persistence::FilePublishMode::Replace, 4096, state_->faults);
        {
            std::lock_guard<std::mutex> lock(state_->projection);
            auto& slot = state_->slots[index];
            slot.retained_temporary = std::move(published.retained_temporary);
            if (!published.status.ok()) {
                return std::move(published.status);
            }
            slot.current = std::move(slot.pending);
            slot.pending.reset();
            slot.phase = SlotPhase::Occupied;
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        state_->protection.store(StatusCode::Unavailable);
        return Status(StatusCode::Unavailable, {});
    }
}

Result<void> LedgerStore::collect_deleted(std::uint64_t segment_id) {
    return remove_authority(segment_id, LedgerState::Deleted);
}

Result<void> LedgerStore::abort_committing(
    std::uint64_t segment_id, const std::function<Result<void>(const LedgerRecord&)>& cleanup) {
    if (!cleanup) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return remove_authority(segment_id, LedgerState::Committing, cleanup);
}

Result<void> LedgerStore::remove_authority(
    std::uint64_t segment_id, LedgerState expected_state,
    const std::function<Result<void>(const LedgerRecord&)>& cleanup) {
    const auto removing =
        expected_state == LedgerState::Deleted ? SlotPhase::Collecting : SlotPhase::Aborting;
    std::unique_lock<std::mutex> flight(state_->execution, std::try_to_lock);
    if (!flight.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    try {
        std::size_t index = 0;
        std::optional<LedgerRecord> expected;
        bool retry = false;
        {
            std::lock_guard<std::mutex> lock(state_->projection);
            index = state_->find(segment_id);
            if (index == state_->slots.size()) {
                return Status(StatusCode::NotFound, {});
            }
            const auto& slot = state_->slots[index];
            if ((slot.phase != SlotPhase::Occupied && slot.phase != removing) || !slot.current ||
                slot.current->state != expected_state || slot.token_present || slot.pending ||
                !slot.retained_temporary.empty()) {
                return Status(StatusCode::Conflict, {});
            }
            retry = slot.phase == removing;
            expected = slot.current;
        }
        auto valid = state_->verify_namespace(segment_id);
        if (!valid.ok()) {
            return valid.status();
        }
        const auto parent = state_->directory / shard_name(segment_id);
        auto actual = read_record(parent, segment_id);
        if (actual.ok()) {
            auto same = same_record(actual.value(), *expected);
            if (!same.ok()) {
                return same.status();
            }
            if (!same.value()) {
                return Status(StatusCode::DataLoss, {});
            }
        } else if (!retry || actual.status().code() != StatusCode::NotFound) {
            return actual.status().code() == StatusCode::NotFound ? Status(StatusCode::DataLoss, {})
                                                                  : actual.status();
        }
        if (cleanup) {
            auto cleaned = cleanup(*expected);
            if (!cleaned.ok()) {
                return cleaned.status();
            }
            valid = state_->verify_namespace(segment_id);
            if (!valid.ok()) {
                return valid.status();
            }
        }
        {
            std::lock_guard<std::mutex> lock(state_->projection);
            state_->slots[index].phase = removing;
        }
        if (actual.ok()) {
            if ((state_->faults && state_->faults(persistence::FileStep::RemoveAuthority) != 0) ||
                unlink((parent / file_name(segment_id)).c_str()) != 0) {
                return Status(StatusCode::Unavailable, {});
            }
        }
        valid = persistence::sync_directory(parent, state_->faults);
        if (!valid.ok()) {
            return valid.status();
        }
        valid = state_->verify_namespace(segment_id);
        if (!valid.ok()) {
            return valid.status();
        }
        if (cleanup) {
            auto confirmed = cleanup(*expected);
            if (!confirmed.ok()) {
                return confirmed.status();
            }
        }
        {
            std::lock_guard<std::mutex> lock(state_->projection);
            auto& slot = state_->slots[index];
            slot.current.reset();
            slot.phase = SlotPhase::Empty;
            slot.identity = 0;
            // Preserve the registration generation across slot reuse.
            --state_->slots_in_use;
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        state_->protection.store(StatusCode::Unavailable);
        return Status(StatusCode::Unavailable, {});
    } catch (...) {
        return Status(StatusCode::Internal, {});
    }
}

Result<void> LedgerStore::reconcile(std::uint64_t segment_id) {
    std::unique_lock<std::mutex> flight(state_->execution, std::try_to_lock);
    if (!flight.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    try {
        std::size_t index = 0;
        std::optional<LedgerRecord> previous;
        std::optional<LedgerRecord> pending;
        std::string retained;
        {
            std::lock_guard<std::mutex> lock(state_->projection);
            index = state_->find(segment_id);
            if (index == state_->slots.size()) {
                return Status(StatusCode::NotFound, {});
            }
            const auto& slot = state_->slots[index];
            if (slot.phase == SlotPhase::Collecting || slot.phase == SlotPhase::Aborting) {
                // Only the maintenance owner can renew the filesystem proof
                // and retry removal. Generic reconciliation cannot authorize it.
                return Status(StatusCode::Unavailable, {});
            }
            if (slot.phase != SlotPhase::Uncertain) {
                return {};
            }
            previous = slot.current;
            pending = slot.pending;
            retained = slot.retained_temporary;
        }
        auto valid = state_->verify_namespace(segment_id);
        if (!valid.ok()) {
            return valid.status();
        }
        const auto parent = state_->directory / shard_name(segment_id);
        auto actual = read_record(parent, segment_id);
        if (!actual.ok()) {
            if (actual.status().code() != StatusCode::NotFound || previous) {
                return actual.status().code() == StatusCode::NotFound
                           ? Status(StatusCode::DataLoss, {})
                           : actual.status();
            }
        } else {
            bool recognized = false;
            for (const auto* known : {&previous, &pending}) {
                if (!*known) {
                    continue;
                }
                auto same = same_record(actual.value(), **known);
                if (!same.ok()) {
                    return same.status();
                }
                recognized = recognized || same.value();
            }
            if (!recognized) {
                return Status(StatusCode::DataLoss, {});
            }
            valid = persistence::sync_file(parent, file_name(segment_id), state_->faults);
            if (!valid.ok()) {
                return valid.status();
            }
        }
        if (!retained.empty() && unlink((parent / retained).c_str()) != 0 && errno != ENOENT) {
            return Status(StatusCode::Unavailable, {});
        }
        valid = persistence::sync_directory(parent, state_->faults);
        if (!valid.ok()) {
            return valid.status();
        }
        {
            std::lock_guard<std::mutex> lock(state_->projection);
            auto& slot = state_->slots[index];
            slot.pending.reset();
            slot.retained_temporary.clear();
            if (actual.ok()) {
                slot.current.emplace(std::move(actual.value()));
                slot.phase = SlotPhase::Occupied;
            } else if (slot.token_present) {
                slot.phase = SlotPhase::Reserved;
            } else {
                slot.phase = SlotPhase::Empty;
                slot.identity = 0;
                --state_->slots_in_use;
            }
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        state_->protection.store(StatusCode::Unavailable);
        return Status(StatusCode::Unavailable, {});
    }
}

Result<LedgerRecord> LedgerStore::get(std::uint64_t segment_id) const {
    if (state_->protection.load() != StatusCode::Ok) {
        return Status(state_->protection.load(), {});
    }
    try {
        std::lock_guard<std::mutex> lock(state_->projection);
        const auto index = state_->find(segment_id);
        if (index == state_->slots.size()) {
            return Status(StatusCode::NotFound, {});
        }
        const auto& slot = state_->slots[index];
        if (slot.phase == SlotPhase::Uncertain || slot.phase == SlotPhase::Collecting ||
            slot.phase == SlotPhase::Aborting) {
            return Status(StatusCode::Unavailable, {});
        }
        if (!slot.current) {
            return Status(StatusCode::NotFound, {});
        }
        return *slot.current;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::vector<LedgerRecord>> LedgerStore::recovery_snapshot() const {
    if (state_->protection.load() != StatusCode::Ok) {
        return Status(state_->protection.load(), {});
    }
    try {
        std::lock_guard<std::mutex> lock(state_->projection);
        std::vector<LedgerRecord> records;
        records.reserve(state_->slots_in_use);
        for (const auto& slot : state_->slots) {
            if (slot.phase == SlotPhase::Uncertain || slot.phase == SlotPhase::Collecting ||
                slot.phase == SlotPhase::Aborting) {
                return Status(StatusCode::Unavailable, {});
            }
            if (slot.current) {
                records.push_back(*slot.current);
            }
        }
        return records;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

std::uint32_t LedgerStore::capacity() const noexcept {
    return static_cast<std::uint32_t>(state_->slots.size());
}

StatusCode LedgerStore::protection() const noexcept { return state_->protection.load(); }

}  // namespace rodaq::artifact
