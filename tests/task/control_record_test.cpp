#include "task/control_record.hpp"
#include "task/enum_codec.hpp"
#include "persistence/json.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <string>

namespace {

namespace task = rodaq::task;
namespace persistence = rodaq::persistence;
using namespace std::chrono_literals;
using rodaq::DesiredState;
using rodaq::OperationKind;
using rodaq::OperationState;
using rodaq::StatusCode;

const std::string kDigest = "sha256:" + std::string(64, 'a');

task::TaskControlRecord record() {
    task::TaskControlRecord value;
    value.task_id = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
    value.active_config_version = kDigest;
    return value;
}

task::StoredOperation operation() {
    task::StoredOperation value;
    value.request = {"start-1"};
    value.request_digest = kDigest;
    value.snapshot.operation_id =
        rodaq::parse_operation_id("01234567-89ab-4cde-8fab-0123456789ac").value();
    value.snapshot.kind = OperationKind::Start;
    value.snapshot.control_generation = 2;
    value.snapshot.accepted_at = rodaq::WallTime{123ns};
    return value;
}

persistence::Json document(const task::TaskControlRecord& value) {
    auto encoded = task::encode_control_record(value, {});
    EXPECT_TRUE(encoded.ok());
    return persistence::Json::parse(encoded.value());
}

rodaq::Result<task::TaskControlRecord> decode(persistence::Json value, bool integrity = true) {
    auto bytes = integrity ? persistence::encode_integrity(value, "rodaq.task-control.v1")
                           : rodaq::Result<std::string>{value.dump()};
    EXPECT_TRUE(bytes.ok());
    return task::decode_control_record(bytes.value(), {});
}

TEST(TaskControlRecord, InitialAuthorityUsesScalarFieldsAndIndependentIntegrityGolden) {
    auto value = record();
    const auto json = document(value);
    EXPECT_TRUE(json.at("current_operation_id").is_null());
    EXPECT_TRUE(json.at("reset_intent").is_null());
    EXPECT_EQ(json.at("control_generation"), "1");
    EXPECT_EQ(json.at("record_revision"), "1");
    EXPECT_EQ(json.at("desired_state"), "stopped");
    EXPECT_EQ(
        json.at("content_digest"),
        "sha256:2017fba0524c575a7bd1b23c0460a27715fa95ce4af2419d4f8e8656dc6d26e6");
    auto loaded = decode(json);
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value().task_id, value.task_id);
    EXPECT_EQ(loaded.value().control_generation, 1);
    EXPECT_TRUE(loaded.value().operation_history.entries.empty());
}

TEST(TaskControlRecord, FullWidthCountersAndAllOperationStatesRoundTripWithoutNumberConversion) {
    auto value = record();
    value.control_generation = std::numeric_limits<std::uint64_t>::max();
    value.record_revision = value.control_generation;
    value.reset_generation = value.control_generation - 1;
    value.desired_state = DesiredState::Running;
    value.operation_history.entries.push_back(operation());
    auto& snapshot = value.operation_history.entries[0].snapshot;
    snapshot.control_generation = value.control_generation;
    value.current_operation_id = snapshot.operation_id;
    for (auto state :
         {OperationState::Accepted, OperationState::InProgress, OperationState::Succeeded,
          OperationState::PartiallySucceeded, OperationState::Failed, OperationState::Superseded}) {
        snapshot.state = state;
        snapshot.terminal_at = task::terminal(state)
                                   ? std::optional<rodaq::WallTime>{rodaq::WallTime{-1ns}}
                                   : std::nullopt;
        const auto json = document(value);
        const auto& entry = json.at("operation_history")[0];
        EXPECT_TRUE(entry.at("state").is_string());
        EXPECT_EQ(entry.at("terminal_result").is_null(), !task::terminal(state));
        auto loaded = decode(json);
        ASSERT_TRUE(loaded.ok());
        EXPECT_EQ(loaded.value().record_revision, value.record_revision);
        EXPECT_EQ(loaded.value().reset_generation, value.reset_generation);
        EXPECT_EQ(loaded.value().operation_history.entries[0].snapshot.state, state);
    }
}

TEST(TaskControlRecord, ResetIntentOwnsFixedTaskScopeAndCannotSurviveTerminalOrWrongGeneration) {
    auto value = record();
    value.control_generation = 2;
    value.record_revision = 2;
    value.reset_generation = 1;
    auto reset = operation();
    reset.snapshot.kind = OperationKind::Reset;
    value.operation_history.entries.push_back(reset);
    value.current_operation_id = reset.snapshot.operation_id;
    EXPECT_FALSE(task::encode_control_record(value, {}).ok());
    value.reset_intent = task::ResetIntent{reset.snapshot.operation_id, 1, 1};
    auto json = document(value);
    EXPECT_EQ(json.at("reset_intent").at("scope"), "all_task_artifacts");
    EXPECT_TRUE(decode(json).ok());
    json["reset_intent"]["target_control_generation"] = "2";
    EXPECT_EQ(decode(json).status().code(), StatusCode::DataLoss);
    value.reset_intent->reset_generation = 2;
    EXPECT_FALSE(task::encode_control_record(value, {}).ok());
    value.reset_intent->reset_generation = 1;
    value.operation_history.entries[0].snapshot.state = OperationState::Succeeded;
    value.operation_history.entries[0].snapshot.terminal_at = rodaq::WallTime{2s};
    EXPECT_FALSE(task::encode_control_record(value, {}).ok());
    value.reset_intent.reset();
    EXPECT_TRUE(task::encode_control_record(value, {}).ok());
}

TEST(TaskControlRecord, UnknownFieldsVersionsEnumsNumericTypesAndDigestCorruptionFailClosed) {
    const auto initial = document(record());
    for (const auto* field :
         {"format_version", "task_id", "record_revision", "operation_history", "content_digest"}) {
        auto missing = initial;
        missing.erase(field);
        EXPECT_EQ(decode(missing, false).status().code(), StatusCode::DataLoss) << field;
    }
    auto invalid = initial;
    invalid["extra"] = true;
    EXPECT_EQ(decode(invalid).status().code(), StatusCode::DataLoss);
    invalid = initial;
    invalid["format_version"] = "2.0";
    EXPECT_EQ(decode(invalid).status().code(), StatusCode::Unavailable);
    invalid = initial;
    invalid["desired_state"] = "retired";
    EXPECT_EQ(decode(invalid).status().code(), StatusCode::DataLoss);
    invalid = initial;
    invalid["control_generation"] = 1;
    EXPECT_EQ(decode(invalid).status().code(), StatusCode::DataLoss);
    invalid = initial;
    invalid["record_revision"] = "0";
    EXPECT_EQ(decode(invalid).status().code(), StatusCode::DataLoss);
    invalid = initial;
    invalid["desired_state"] = "running";
    EXPECT_EQ(decode(invalid, false).status().code(), StatusCode::DataLoss);
    auto encoded = task::encode_control_record(record(), {});
    ASSERT_TRUE(encoded.ok());
    auto duplicate = encoded.value();
    duplicate.insert(1, "\"control_generation\":\"99\",");
    EXPECT_EQ(task::decode_control_record(duplicate, {}).status().code(), StatusCode::DataLoss);
}

TEST(TaskControlRecord, CurrentOperationAndTerminalEvidenceAreNotIndependentMutableAuthorities) {
    auto value = record();
    value.control_generation = 2;
    value.record_revision = 2;
    value.operation_history.entries.push_back(operation());
    EXPECT_FALSE(task::encode_control_record(value, {}).ok());
    value.current_operation_id = value.operation_history.entries[0].snapshot.operation_id;
    auto json = document(value);
    json["operation_history"][0]["terminal_result"] = "succeeded";
    EXPECT_EQ(decode(json).status().code(), StatusCode::DataLoss);
    json = document(value);
    json["operation_history"][0]["state"] = "unknown";
    EXPECT_EQ(decode(json).status().code(), StatusCode::DataLoss);
    value.operation_history.entries[0].snapshot.control_generation = 3;
    EXPECT_FALSE(task::encode_control_record(value, {}).ok());
}

TEST(TaskControlRecord, HistoryBudgetIncludesWorstCaseEscapesAndConfiguredCardinality) {
    auto value = record();
    task::HistoryPolicy policy{128, 4, 24h};
    value.control_generation = 128;
    value.record_revision = 128;
    for (unsigned index = 1; index <= 128; ++index) {
        auto entry = operation();
        entry.request.request_key = std::string(100, '\\') + std::to_string(index);
        entry.snapshot.operation_id.value[14] = static_cast<std::uint8_t>(index >> 8u);
        entry.snapshot.operation_id.value[15] = static_cast<std::uint8_t>(index);
        entry.snapshot.control_generation = index;
        entry.snapshot.state = OperationState::Succeeded;
        entry.snapshot.terminal_at = rodaq::WallTime{2s};
        entry.snapshot.diagnostic = std::string(256, '\0');
        value.operation_history.entries.push_back(std::move(entry));
    }
    auto bytes = task::encode_control_record(value, policy);
    ASSERT_TRUE(bytes.ok());
    EXPECT_LE(bytes.value().size(), task::control_record_max_bytes(policy));
    auto loaded = task::decode_control_record(bytes.value(), policy);
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value().operation_history.entries.size(), 128);
    EXPECT_EQ(
        loaded.value().operation_history.entries[0].snapshot.diagnostic, std::string(256, '\0'));
}

}  // namespace
