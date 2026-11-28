#include "task/operation_history.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

namespace task = rodaq::task;
using namespace std::chrono_literals;
using rodaq::OperationKind;
using rodaq::OperationState;
using rodaq::StatusCode;
using rodaq::WallTime;

rodaq::TaskId task_id() {
    return rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
}

rodaq::OperationSnapshot make_op(
    std::uint64_t generation, OperationKind kind = OperationKind::Start) {
    rodaq::OperationSnapshot snap;
    snap.operation_id.value[15] = static_cast<std::uint8_t>(generation);
    snap.control_generation = generation;
    snap.kind = kind;
    snap.state = OperationState::Accepted;
    snap.accepted_at = WallTime{1s};
    return snap;
}

TEST(OperationRegistry, TypedDuplicateSuppressionAndConflictOnChangedSignature) {
    task::OperationRegistry reg({8, 1h});
    task::RequestSignature sig{OperationKind::Start, {true, 1}, std::nullopt};
    const auto op = make_op(2, OperationKind::Start);
    ASSERT_TRUE(reg.record("req-1", sig, op, WallTime{1s}).ok());

    auto found = reg.find_request("req-1", sig, WallTime{2s});
    ASSERT_TRUE(found.ok());
    EXPECT_EQ(found.value(), op.operation_id);

    task::RequestSignature diff{OperationKind::Stop, {true, 1}, std::nullopt};
    EXPECT_EQ(reg.find_request("req-1", diff, WallTime{2s}).status().code(), StatusCode::Conflict);
    EXPECT_EQ(reg.find_request("unknown", sig, WallTime{2s}).status().code(), StatusCode::NotFound);
}

TEST(OperationRegistry, EvictsOldestTerminalEntriesByCountAndTtlAndPreservesKeysetPagination) {
    task::OperationRegistry reg({2, 10s});
    for (std::uint64_t i = 1; i <= 3; ++i) {
        auto op = make_op(i);
        op.state = OperationState::Succeeded;
        op.terminal_at = WallTime{std::chrono::seconds(i)};
        ASSERT_TRUE(reg.record(
                           "req-" + std::to_string(i), {OperationKind::Start, {}, std::nullopt}, op,
                           WallTime{std::chrono::seconds(i)})
                        .ok());
    }
    auto page1 = reg.list(task_id(), {"", 1}, WallTime{4s});
    ASSERT_TRUE(page1.ok());
    ASSERT_EQ(page1.value().items.size(), 1U);
    EXPECT_EQ(page1.value().items[0].control_generation, 3ULL);
    EXPECT_TRUE(page1.value().has_more);
    EXPECT_TRUE(page1.value().history_truncated);

    auto page2 = reg.list(task_id(), {page1.value().next_cursor, 2}, WallTime{4s});
    ASSERT_TRUE(page2.ok());
    ASSERT_EQ(page2.value().items.size(), 1U);
    EXPECT_EQ(page2.value().items[0].control_generation, 2ULL);
    EXPECT_FALSE(page2.value().has_more);

    // After TTL elapses (terminal_at + 10s), expired entries are pruned.
    auto expired =
        reg.find_request("req-2", {OperationKind::Start, {}, std::nullopt}, WallTime{20s});
    EXPECT_EQ(expired.status().code(), StatusCode::NotFound);
}

TEST(CreateRegistry, SuppressesDuplicatesAndRejectsConflictingConfig) {
    task::CreateRegistry reg({4, 1h});
    rodaq::TaskConfig cfg1;
    cfg1.name = "a";
    rodaq::TaskConfig cfg2;
    cfg2.name = "b";
    const auto id = task_id();
    ASSERT_TRUE(reg.record("create-1", cfg1, id, WallTime{1s}).ok());
    EXPECT_EQ(reg.find("create-1", cfg1, WallTime{2s}).value(), id);
    EXPECT_EQ(reg.find("create-1", cfg2, WallTime{2s}).status().code(), StatusCode::Conflict);
    EXPECT_EQ(reg.find("missing", cfg1, WallTime{2s}).status().code(), StatusCode::NotFound);
}

}  // namespace
