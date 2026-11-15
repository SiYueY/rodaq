#include "task/operation_history.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
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

task::StoredOperation operation(
    std::uint64_t generation, OperationKind kind = OperationKind::Start) {
    task::StoredOperation value;
    value.request.request_key = "request-" + std::to_string(generation);
    value.request_digest = "sha256:" + std::string(64, 'a');
    value.snapshot.operation_id.value[15] = static_cast<std::uint8_t>(generation);
    value.snapshot.control_generation = generation;
    value.snapshot.kind = kind;
    value.snapshot.accepted_at = WallTime{1s};
    return value;
}

TEST(OperationHistory, IdempotencyPrecedesCurrentGenerationAndRejectsChangedDigest) {
    task::OperationHistory history{{operation(1)}, false};
    ASSERT_TRUE(task::validate_history(history, {}).ok());
    auto matched = task::find_request(history, {"request-1"}, history.entries[0].request_digest);
    ASSERT_TRUE(matched.ok());
    EXPECT_EQ(matched.value(), history.entries[0].snapshot.operation_id);
    history.entries[0].snapshot.state = OperationState::Superseded;
    history.entries[0].snapshot.terminal_at = WallTime{2s};
    EXPECT_TRUE(task::find_request(history, {"request-1"}, history.entries[0].request_digest).ok());
    EXPECT_EQ(
        task::find_request(history, {"request-1"}, "sha256:" + std::string(64, 'b'))
            .status()
            .code(),
        StatusCode::Conflict);
    EXPECT_EQ(
        task::find_request(history, {"unknown"}, history.entries[0].request_digest).status().code(),
        StatusCode::NotFound);
}

TEST(OperationHistory, ReserveProtectsPauseStopResetAndNeverEvictsLiveOrCurrentOperations) {
    task::HistoryPolicy policy{4, 2, 10s};
    task::OperationHistory history{{operation(1), operation(2)}, false};
    EXPECT_EQ(
        task::append_operation(history, operation(3), {}, policy, WallTime{100s}).status().code(),
        StatusCode::ResourceExhausted);
    for (auto kind : {OperationKind::Pause, OperationKind::Stop, OperationKind::Reset}) {
        auto reserved =
            task::append_operation(history, operation(3, kind), {}, policy, WallTime{100s});
        ASSERT_TRUE(reserved.ok());
        EXPECT_EQ(reserved.value().entries.size(), 3);
        EXPECT_EQ(history.entries.size(), 2);
    }
    for (auto& stored : history.entries) {
        stored.snapshot.state = OperationState::Succeeded;
        stored.snapshot.terminal_at = WallTime{5s};
    }
    auto next = task::append_operation(
        history, operation(3), history.entries[1].snapshot.operation_id, policy, WallTime{100s});
    ASSERT_TRUE(next.ok());
    EXPECT_TRUE(next.value().history_truncated);
    ASSERT_EQ(next.value().entries.size(), 2);
    EXPECT_EQ(next.value().entries.front().snapshot.control_generation, 2);
    EXPECT_FALSE(history.history_truncated);
    EXPECT_EQ(history.entries.front().snapshot.control_generation, 1);
}

TEST(OperationHistory, RetentionUsesTerminalTimeAndCheckedFullSignedClockSpan) {
    task::HistoryPolicy policy{2, 1, 10s};
    auto stored = operation(1);
    stored.snapshot.state = OperationState::Failed;
    stored.snapshot.terminal_at = WallTime{50s};
    task::OperationHistory history{{stored}, false};
    EXPECT_FALSE(task::append_operation(history, operation(2), {}, policy, WallTime{59s}).ok());
    EXPECT_FALSE(task::append_operation(history, operation(2), {}, policy, WallTime{-1ns}).ok());
    EXPECT_TRUE(task::append_operation(history, operation(2), {}, policy, WallTime{60s}).ok());
    history.entries[0].snapshot.terminal_at = WallTime{std::chrono::nanoseconds::min()};
    EXPECT_TRUE(task::append_operation(
                    history, operation(2), {}, policy, WallTime{std::chrono::nanoseconds::max()})
                    .ok());
}

TEST(OperationHistory, GenerationKeyAndIdentityNeverDuplicateAcrossRetainedEntries) {
    task::OperationHistory history{{operation(1)}, false};
    EXPECT_EQ(
        task::append_operation(history, operation(1), {}, {}, {}).status().code(),
        StatusCode::Conflict);
    auto duplicate = operation(2);
    duplicate.request = history.entries[0].request;
    EXPECT_FALSE(task::append_operation(history, duplicate, {}, {}, {}).ok());
    duplicate = operation(2);
    duplicate.snapshot.operation_id = history.entries[0].snapshot.operation_id;
    EXPECT_FALSE(task::append_operation(history, duplicate, {}, {}, {}).ok());
    history.entries.push_back(operation(2));
    history.entries[1].snapshot.control_generation = 1;
    EXPECT_FALSE(task::validate_history(history, {}).ok());
    EXPECT_FALSE(task::validate_history({}, {4, 4, 1s}).ok());
    EXPECT_FALSE(task::validate_history({}, {4, 1, -1ns}).ok());
}

TEST(OperationHistory, ClosedEnumsOwnedUtf8BoundsAndTerminalEvidenceAreValidated) {
    const auto baseline = operation(1);
    auto value = baseline;
    value.snapshot.state = OperationState::Failed;
    EXPECT_FALSE(task::validate_operation(value).ok());
    value.snapshot.terminal_at = WallTime{0ns};
    EXPECT_TRUE(task::validate_operation(value).ok());
    value.snapshot.state = OperationState::Accepted;
    EXPECT_FALSE(task::validate_operation(value).ok());
    value = baseline;
    value.snapshot.retryability = static_cast<rodaq::Retryability>(-1);
    EXPECT_FALSE(task::validate_operation(value).ok());
    value = baseline;
    value.snapshot.failure_domain = static_cast<rodaq::FailureDomain>(8);
    EXPECT_FALSE(task::validate_operation(value).ok());
    value = baseline;
    value.snapshot.data_impact = static_cast<rodaq::DataImpact>(6);
    EXPECT_FALSE(task::validate_operation(value).ok());
    value = baseline;
    value.snapshot.kind = static_cast<OperationKind>(7);
    EXPECT_FALSE(task::validate_operation(value).ok());
    value = baseline;
    value.snapshot.state = static_cast<OperationState>(6);
    EXPECT_FALSE(task::validate_operation(value).ok());
    value = baseline;
    value.snapshot.diagnostic = std::string(257, 'x');
    EXPECT_FALSE(task::validate_operation(value).ok());
    value.snapshot.diagnostic = std::string("\xc0\x80", 2);
    EXPECT_FALSE(task::validate_operation(value).ok());
}

TEST(OperationHistory, KeysetCursorSurvivesEvictionAndAppendWithoutOffsetDuplicates) {
    task::OperationHistory history{{operation(1), operation(2), operation(3)}, false};
    auto first = task::operation_page(task_id(), history, {"", 1});
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first.value().items.size(), 1);
    EXPECT_EQ(first.value().items[0].control_generation, 1);
    EXPECT_TRUE(first.value().has_more);
    const auto cursor = first.value().next_cursor;
    history.entries.erase(history.entries.begin());
    history.history_truncated = true;
    history.entries.push_back(operation(4));
    auto second = task::operation_page(task_id(), history, {cursor, 2});
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second.value().items.size(), 2);
    EXPECT_EQ(second.value().items[0].control_generation, 2);
    EXPECT_EQ(second.value().items[1].control_generation, 3);
    EXPECT_TRUE(second.value().has_more);
    EXPECT_TRUE(second.value().history_truncated);
    EXPECT_EQ(second.value().oldest_retained_control_generation, 2);
    auto third = task::operation_page(task_id(), history, {second.value().next_cursor, 2});
    ASSERT_TRUE(third.ok());
    ASSERT_EQ(third.value().items.size(), 1);
    EXPECT_EQ(third.value().items[0].control_generation, 4);
    EXPECT_FALSE(third.value().has_more);
    EXPECT_TRUE(third.value().next_cursor.empty());
    auto other_task = task_id();
    ++other_task.value[15];
    EXPECT_FALSE(task::operation_page(other_task, history, {cursor, 1}).ok());
    EXPECT_FALSE(task::operation_page(task_id(), history, {"tasks:v1:bad", 1}).ok());
    EXPECT_FALSE(task::operation_page(task_id(), history, {"", 0}).ok());
    EXPECT_FALSE(task::operation_page(task_id(), history, {"", 257}).ok());
}

}  // namespace
