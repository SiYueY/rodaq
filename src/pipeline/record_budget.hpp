#pragma once

#include <cstdint>
#include <memory>

#include <rodaq/result.hpp>

namespace rodaq::pipeline {

struct RecordBudgetSnapshot {
    std::uint64_t records{0};
    std::uint64_t logical_bytes{0};
    std::uint64_t high_water_records{0};
    std::uint64_t high_water_logical_bytes{0};
};

struct RecordBudgetState;
class RecordBudget;

// One pre-admission record/logical-byte charge. After acceptance the Record
// owns it through actual Writer access/settlement, independently of queue slots.
class RecordCharge {
public:
    RecordCharge() noexcept = default;
    RecordCharge(const RecordCharge&) = delete;
    RecordCharge& operator=(const RecordCharge&) = delete;
    RecordCharge(RecordCharge&& other) noexcept;
    RecordCharge& operator=(RecordCharge&& other) noexcept;
    ~RecordCharge() noexcept;

    explicit operator bool() const noexcept;
    std::uint64_t logical_bytes() const noexcept;
    void reset() noexcept;

private:
    friend class RecordBudget;
    RecordCharge(std::shared_ptr<RecordBudgetState> state, std::uint64_t bytes) noexcept;
    std::shared_ptr<RecordBudgetState> state_;
    std::uint64_t bytes_{0};
};

class RecordBudget {
public:
    static Result<RecordBudget> create(std::uint64_t max_records, std::uint64_t max_logical_bytes);
    RecordBudget(const RecordBudget&) = delete;
    RecordBudget& operator=(const RecordBudget&) = delete;
    RecordBudget(RecordBudget&&) noexcept = default;
    RecordBudget& operator=(RecordBudget&&) noexcept = default;

    Result<RecordCharge> reserve(std::uint64_t logical_bytes);
    RecordBudgetSnapshot snapshot() const;

private:
    explicit RecordBudget(std::shared_ptr<RecordBudgetState> state) noexcept;
    std::shared_ptr<RecordBudgetState> state_;
};

}  // namespace rodaq::pipeline
