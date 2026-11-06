#include <rodaq/runtime/runtime.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>

namespace rodaq::runtime {
namespace {

TEST(RuntimeConfigTest, DefaultsExposeOnlyTheMultiClassPayloadModel) {
    RuntimeConfig config;
    ASSERT_EQ(1U, config.resources.payload_pool.classes.size());
    EXPECT_EQ(4U * 1024U * 1024U, config.resources.payload_pool.classes[0].max_payload_bytes);
    EXPECT_GT(config.resources.payload_pool.classes[0].block_count, 0U);
}

TEST(RuntimeConfigTest, RejectsEmptyOrIncompleteResourceConfiguration) {
    RuntimeConfig empty_classes;
    empty_classes.resources.payload_pool.classes.clear();
    empty_classes.target_uri = "file://unused";
    Runtime empty_runtime(std::move(empty_classes));
    auto empty_result = empty_runtime.configure();
    ASSERT_FALSE(empty_result.ok());
    EXPECT_EQ(StatusCode::InvalidArgument, empty_result.status().status_code());
}

TEST(RuntimeConfigTest, ValidatesPayloadClassCoverageAndCapacityConsistency) {
    RuntimeConfig uncovered;
    uncovered.target_uri = "file://unused";
    uncovered.resources.payload_pool.classes = {PayloadClassConfig{1024, 4}};
    uncovered.resources.max_single_payload_bytes = 2048;
    Runtime unfinished(std::move(uncovered));
    auto uncovered_result = unfinished.configure();
    ASSERT_FALSE(uncovered_result.ok());
    EXPECT_EQ(StatusCode::InvalidArgument, uncovered_result.status().status_code());

    RuntimeConfig mismatched;
    mismatched.target_uri = "file://unused";
    mismatched.resources.dispatch_capacity = 8;
    mismatched.resources.operation_capacity = 7;
    Runtime mismatch_runtime(std::move(mismatched));
    auto mismatch_result = mismatch_runtime.configure();
    ASSERT_FALSE(mismatch_result.ok());
    EXPECT_EQ(StatusCode::InvalidArgument, mismatch_result.status().status_code());
}

}  // namespace
}  // namespace rodaq::runtime
