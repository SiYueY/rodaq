#include "task/identity.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <type_traits>

namespace {

static_assert(!std::is_convertible_v<rodaq::TaskId, rodaq::OperationId>);

TEST(TaskIdentity, CanonicalNonzeroUuidAndOwnedRandomIdentities) {
    const std::string text = "01234567-89ab-4cde-8fab-0123456789ab";
    auto task = rodaq::parse_task_id(text);
    auto operation = rodaq::parse_operation_id(text);
    ASSERT_TRUE(task.ok());
    ASSERT_TRUE(operation.ok());
    EXPECT_EQ(rodaq::to_string(task.value()), text);
    EXPECT_EQ(rodaq::to_string(operation.value()), text);
    for (const auto* invalid :
         {"", "00000000-0000-0000-0000-000000000000", "01234567-89ab-4Cde-8fab-0123456789ab",
          "01234567_89ab-4cde-8fab-0123456789ab", "01234567-89ab-4cde-8fab-0123456789ab\n"}) {
        EXPECT_FALSE(rodaq::parse_task_id(invalid).ok());
        EXPECT_FALSE(rodaq::parse_operation_id(invalid).ok());
    }
    std::set<rodaq::TaskId> identities;
    std::set<std::string> versions;
    for (unsigned count = 0; count < 128; ++count) {
        auto generated = rodaq::task::new_task_id();
        ASSERT_TRUE(generated.ok());
        EXPECT_TRUE(identities.insert(generated.value()).second);
        EXPECT_EQ(generated.value().value[6] >> 4u, 4);
        EXPECT_EQ(generated.value().value[8] >> 6u, 2);
        EXPECT_TRUE(rodaq::parse_task_id(rodaq::to_string(generated.value())).ok());

        auto ver = rodaq::task::new_config_version();
        ASSERT_TRUE(ver.ok());
        EXPECT_TRUE(versions.insert(ver.value()).second);
    }
    EXPECT_TRUE(rodaq::task::new_operation_id().ok());
}

TEST(TaskRequest, ExactVisibleAsciiRequestKeyValidation) {
    EXPECT_TRUE(rodaq::task::validate_request_identity({"Example-1"}).ok());
    EXPECT_TRUE(rodaq::task::validate_request_identity({std::string(128, '~')}).ok());
    for (const auto& invalid :
         {std::string{}, std::string(129, 'a'), std::string("a b"), std::string("a\tb"),
          std::string("a\0b", 3), std::string("键")}) {
        EXPECT_FALSE(rodaq::task::validate_request_identity({invalid}).ok());
    }
}

}  // namespace
