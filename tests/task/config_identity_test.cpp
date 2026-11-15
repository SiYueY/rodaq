#include "task/config_identity.hpp"
#include "task/config.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <limits>
#include <string>

namespace {

namespace task = rodaq::task;
using rodaq::StatusCode;

rodaq::TaskConfig config() {
    std::ifstream input(RODAQ_TASK_CONFIG_FIXTURE);
    auto parsed = rodaq::parse_task_config(rodaq::persistence::Json::parse(input).dump());
    EXPECT_TRUE(parsed.ok());
    return parsed.ok() ? std::move(parsed).value() : rodaq::TaskConfig{};
}

TEST(ConfigIdentity, InitialAllocationIsDeterministicAndPreservesExplicitImportIds) {
    auto request = config();
    request.pipelines[0].pipeline_id = 2;
    request.pipelines[0].streams[0].stream_id = std::numeric_limits<std::uint64_t>::max();
    auto added = request.pipelines[0];
    added.pipeline_id = 0;
    added.streams[0].stream_id = 0;
    request.pipelines.push_back(added);
    auto first = task::assign_initial_config(request, {});
    auto retry = task::assign_initial_config(request, {});
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(first.value().config.pipelines[0].pipeline_id, 2);
    EXPECT_EQ(first.value().config.pipelines[1].pipeline_id, 1);
    EXPECT_EQ(first.value().config.pipelines[1].streams[0].stream_id, 1);
    EXPECT_EQ(first.value().stream_id_high_water, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(first.value().pipeline_id_high_water, 2);
    EXPECT_EQ(
        task::config_version(first.value().config, {}).value(),
        task::config_version(retry.value().config, {}).value());
    EXPECT_EQ(request.pipelines[1].pipeline_id, 0);
    EXPECT_EQ(request.pipelines[1].streams[0].stream_id, 0);
}

TEST(ConfigIdentity, AllOmittedIdsBecomeTaskScopedAndArrayOrderRemainsBusinessOrder) {
    auto request = config();
    request.pipelines[0].pipeline_id = 0;
    request.pipelines[0].streams[0].stream_id = 0;
    request.pipelines[0].streams.push_back(request.pipelines[0].streams[0]);
    request.pipelines[0].streams[1].stream_key = "second";
    request.pipelines.push_back(request.pipelines[0]);
    auto assigned = task::assign_initial_config(request, {});
    ASSERT_TRUE(assigned.ok());
    EXPECT_EQ(assigned.value().config.pipelines[0].pipeline_id, 1);
    EXPECT_EQ(assigned.value().config.pipelines[1].pipeline_id, 2);
    EXPECT_EQ(assigned.value().config.pipelines[0].streams[0].stream_id, 1);
    EXPECT_EQ(assigned.value().config.pipelines[0].streams[1].stream_id, 2);
    EXPECT_EQ(assigned.value().config.pipelines[1].streams[0].stream_id, 3);
    EXPECT_EQ(assigned.value().config.pipelines[1].streams[1].stream_id, 4);
    EXPECT_EQ(assigned.value().pipeline_id_high_water, 2);
    EXPECT_EQ(assigned.value().stream_id_high_water, 4);
}

TEST(ConfigIdentity, ConfigureUsesDurableHighWaterInsteadOfCurrentVisibleMaximum) {
    const auto current = config();
    auto request = current;
    auto stream = request.pipelines[0].streams[0];
    stream.stream_id = 0;
    stream.stream_key = "new";
    request.pipelines[0].streams.push_back(stream);
    auto pipeline = current.pipelines[0];
    pipeline.pipeline_id = 0;
    pipeline.streams[0].stream_id = 0;
    request.pipelines.push_back(pipeline);
    auto assigned = task::assign_config_change(current, request, 40, 70, {});
    ASSERT_TRUE(assigned.ok());
    EXPECT_EQ(assigned.value().config.pipelines[0].pipeline_id, 10);
    EXPECT_EQ(assigned.value().config.pipelines[0].streams[0].stream_id, 20);
    EXPECT_EQ(assigned.value().config.pipelines[0].streams[1].stream_id, 71);
    EXPECT_EQ(assigned.value().config.pipelines[1].pipeline_id, 41);
    EXPECT_EQ(assigned.value().config.pipelines[1].streams[0].stream_id, 72);
    EXPECT_EQ(assigned.value().pipeline_id_high_water, 41);
    EXPECT_EQ(assigned.value().stream_id_high_water, 72);
}

TEST(ConfigIdentity, UnknownRetiredIdsAndSameIdBindingChangesAreRejected) {
    const auto current = config();
    auto request = current;
    request.pipelines[0].pipeline_id = 9;
    EXPECT_EQ(
        task::assign_config_change(current, request, 40, 70, {}).status().code(),
        StatusCode::InvalidArgument);
    request = current;
    request.pipelines[0].streams[0].stream_id = 19;
    EXPECT_EQ(
        task::assign_config_change(current, request, 40, 70, {}).status().code(),
        StatusCode::InvalidArgument);
    request = current;
    request.pipelines[0].streams[0].stream_key = "other-binding";
    EXPECT_EQ(
        task::assign_config_change(current, request, 40, 70, {}).status().code(),
        StatusCode::InvalidArgument);
    request = current;
    request.pipelines[0].streams[0].stream_id = 0;
    auto replaced = task::assign_config_change(current, request, 40, 70, {});
    ASSERT_TRUE(replaced.ok());
    EXPECT_EQ(replaced.value().config.pipelines[0].streams[0].stream_id, 71);
    EXPECT_EQ(
        task::assign_config_change(replaced.value().config, current, 40, 71, {}).status().code(),
        StatusCode::InvalidArgument);
}

TEST(ConfigIdentity, TaskScopedRetainedStreamCanMoveToNewPipelineWithSameOpaqueBinding) {
    const auto current = config();
    auto request = current;
    request.pipelines[0].pipeline_id = 0;
    auto moved = task::assign_config_change(current, request, 10, 20, {});
    ASSERT_TRUE(moved.ok());
    EXPECT_EQ(moved.value().config.pipelines[0].pipeline_id, 11);
    EXPECT_EQ(moved.value().config.pipelines[0].streams[0].stream_id, 20);
}

TEST(ConfigIdentity, ExhaustionNeverWrapsOrPreventsAnOtherwiseUnchangedConfiguration) {
    const auto current = config();
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(task::assign_config_change(current, current, maximum, maximum, {}).ok());
    auto request = current;
    request.pipelines[0].pipeline_id = 0;
    EXPECT_EQ(
        task::assign_config_change(current, request, maximum, maximum, {}).status().code(),
        StatusCode::ResourceExhausted);
    request = current;
    request.pipelines[0].streams[0].stream_id = 0;
    EXPECT_EQ(
        task::assign_config_change(current, request, maximum, maximum, {}).status().code(),
        StatusCode::ResourceExhausted);
    EXPECT_EQ(
        task::assign_config_change(current, current, 9, 20, {}).status().code(),
        StatusCode::DataLoss);
    EXPECT_EQ(
        task::assign_config_change(current, current, 10, 19, {}).status().code(),
        StatusCode::DataLoss);
}

TEST(ConfigIdentity, PipelineReceivesCanonicalBindingsAndFullTaskIdentityWatermarks) {
    auto assigned = task::assign_initial_config(config(), {});
    ASSERT_TRUE(assigned.ok());
    assigned.value().pipeline_id_high_water = 100;
    assigned.value().stream_id_high_water = 200;
    auto projection = task::identity_projection(assigned.value(), {});
    ASSERT_TRUE(projection.ok());
    EXPECT_EQ(
        projection.value().config_version,
        task::config_version(assigned.value().config, {}).value());
    EXPECT_EQ(projection.value().pipeline_id_high_water, 100);
    EXPECT_EQ(projection.value().stream_id_high_water, 200);
    ASSERT_EQ(projection.value().streams.size(), 1);
    EXPECT_EQ(projection.value().streams[0].stream_id, 20);
    EXPECT_EQ(projection.value().streams[0].stream_key, "sample");
    assigned.value().stream_id_high_water = 19;
    EXPECT_FALSE(task::identity_projection(assigned.value(), {}).ok());
}

}  // namespace
