#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include <vector>

#include "ledger_record.hpp"
#include "../persistence/atomic_file.hpp"

namespace rodaq::artifact {

struct LedgerStoreState;
class LedgerStore;

// Prepared registration capacity, acquired before the Segment can accept data.
// Destruction before publication rolls back; uncertain publication retains its
// slot until explicit reconciliation proves authority/absence durably.
class LedgerRegistration {
public:
    LedgerRegistration(const LedgerRegistration&) = delete;
    LedgerRegistration& operator=(const LedgerRegistration&) = delete;
    LedgerRegistration(LedgerRegistration&& other) noexcept;
    LedgerRegistration& operator=(LedgerRegistration&&) = delete;
    ~LedgerRegistration() noexcept;

    std::uint64_t segment_id() const noexcept { return state_ ? segment_id_ : 0; }

private:
    friend class LedgerStore;
    LedgerRegistration(
        std::shared_ptr<LedgerStoreState> state, std::size_t slot, std::uint64_t generation,
        std::uint64_t segment_id) noexcept;
    std::shared_ptr<LedgerStoreState> state_;
    std::size_t slot_;
    std::uint64_t generation_;
    std::uint64_t segment_id_;
};

enum class LedgerOpenMode { Initialize, Existing };

// Exactly one durable Ledger authority. Queries use a separate Catalog; get /
// recovery_snapshot expose only this owner's known durable facts. A persistence
// flight never holds the capacity/projection mutex across I/O or fault hooks.
class LedgerStore {
public:
    static Result<std::unique_ptr<LedgerStore>> open(
        const std::filesystem::path& directory, LedgerOpenMode mode,
        std::uint32_t max_records = 4096, persistence::FileFaultHook faults = {});
    LedgerStore(const LedgerStore&) = delete;
    LedgerStore& operator=(const LedgerStore&) = delete;
    ~LedgerStore() noexcept;

    Result<LedgerRegistration> reserve_registration(std::uint64_t segment_id);
    Result<void> create_committing(LedgerRegistration& registration, const LedgerRecord& record);
    // Caller already owns actual filesystem/integrity/fence/delete proof.
    // This Store enforces durable state monotonicity, not that business proof.
    Result<void> advance(const LedgerRecord& record, LedgerState expected_state);
    // Maintenance caller proves audit expiry, artifact absence/parent durability
    // and absence of publication/recovery references before each attempt.
    // An uncertain unlink retains capacity until this same operation settles.
    Result<void> collect_deleted(std::uint64_t segment_id);
    // Exclusive recovery caller holds a durable publication fence. Idempotent
    // cleanup receives the known pre-commit row before and after its removal,
    // proving file settlement and parent durability each time. It runs on this
    // actual execution domain without projection locks. Uncertainty retains
    // capacity; generic reconciliation cannot discharge the abort owner.
    Result<void> abort_committing(
        std::uint64_t segment_id, const std::function<Result<void>(const LedgerRecord&)>& cleanup);
    Result<void> reconcile(std::uint64_t segment_id);
    Result<LedgerRecord> get(std::uint64_t segment_id) const;
    Result<std::vector<LedgerRecord>> recovery_snapshot() const;
    std::uint32_t capacity() const noexcept;
    StatusCode protection() const noexcept;

private:
    Result<void> remove_authority(
        std::uint64_t segment_id, LedgerState expected,
        const std::function<Result<void>(const LedgerRecord&)>& cleanup = {});
    explicit LedgerStore(std::shared_ptr<LedgerStoreState> state) noexcept;
    std::shared_ptr<LedgerStoreState> state_;
};

}  // namespace rodaq::artifact
