#include "task/config.hpp"
#include "task/identity.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <type_traits>

namespace {

static_assert(!std::is_convertible_v<rodaq::TaskId, rodaq::OperationId>);

rodaq::TaskConfig config() {
    std::ifstream input(RODAQ_TASK_CONFIG_FIXTURE);
    const auto document = rodaq::persistence::Json::parse(input);
    auto parsed = rodaq::parse_task_config(document.dump());
    EXPECT_TRUE(parsed.ok());
    return parsed.ok() ? std::move(parsed).value() : rodaq::TaskConfig{};
}

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
    for (unsigned count = 0; count < 128; ++count) {
        auto generated = rodaq::task::new_task_id();
        ASSERT_TRUE(generated.ok());
        EXPECT_TRUE(identities.insert(generated.value()).second);
        EXPECT_EQ(generated.value().value[6] >> 4u, 4);
        EXPECT_EQ(generated.value().value[8] >> 6u, 2);
        EXPECT_TRUE(rodaq::parse_task_id(rodaq::to_string(generated.value())).ok());
    }
    EXPECT_TRUE(rodaq::task::new_operation_id().ok());
}

TEST(TaskRequest, ExactVisibleKeyBytesAndIndependentRawHashGolden) {
    auto hashed = rodaq::task::request_key_hash({"Example-1"});
    ASSERT_TRUE(hashed.ok());
    EXPECT_EQ(
        hashed.value(), "sha256:bf62124d25c34e780ff3adaed2d625e9d6fe7d56429437bffa27907c9d074c76");
    EXPECT_NE(hashed.value(), rodaq::task::request_key_hash({"example-1"}).value());
    EXPECT_TRUE(rodaq::task::validate_request_identity({std::string(128, '~')}).ok());
    for (const auto& invalid :
         {std::string{}, std::string(129, 'a'), std::string("a b"), std::string("a\tb"),
          std::string("a\0b", 3), std::string("键")}) {
        EXPECT_FALSE(rodaq::task::request_key_hash({invalid}).ok());
    }
}

TEST(TaskRequest, DomainSeparatedSemanticGoldensAndPreconditionCoverage) {
    auto configuration = config();
    auto created = rodaq::task::create_request_digest(configuration, {});
    ASSERT_TRUE(created.ok());
    EXPECT_EQ(
        created.value(), "sha256:35252fa43cbd8fb320b056ae0eadb90978e742981934ca17983a1c4b5457cc45");
    auto version = rodaq::task::config_version(configuration, {});
    ASSERT_TRUE(version.ok());
    EXPECT_NE(created.value(), version.value());

    const auto task = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
    const rodaq::ControlPrecondition guarded{true, std::numeric_limits<std::uint64_t>::max()};
    auto configured = rodaq::task::configure_request_digest(task, configuration, guarded, {});
    ASSERT_TRUE(configured.ok());
    EXPECT_EQ(
        configured.value(),
        "sha256:e9e0cc90969747cec73a8148128cb835450b90a2b419ffc03e3ef9f81322daa4");
    EXPECT_NE(
        configured.value(),
        rodaq::task::configure_request_digest(task, configuration, {}, {}).value());

    auto started = rodaq::task::lifecycle_request_digest(task, rodaq::OperationKind::Start, {});
    ASSERT_TRUE(started.ok());
    EXPECT_EQ(
        started.value(), "sha256:3469b870a2646a84d45bc392938fef490dc5f7f79a7074b5e28c192fdde698d1");
    std::set<std::string> digests;
    for (auto kind :
         {rodaq::OperationKind::Start, rodaq::OperationKind::Pause, rodaq::OperationKind::Resume,
          rodaq::OperationKind::Stop, rodaq::OperationKind::Reset, rodaq::OperationKind::Retire}) {
        auto result = rodaq::task::lifecycle_request_digest(task, kind, {});
        ASSERT_TRUE(result.ok());
        EXPECT_TRUE(digests.insert(result.value()).second);
        EXPECT_NE(
            result.value(), rodaq::task::lifecycle_request_digest(task, kind, guarded).value());
    }
    EXPECT_FALSE(
        rodaq::task::lifecycle_request_digest(task, rodaq::OperationKind::Configure, {}).ok());
    EXPECT_FALSE(rodaq::task::lifecycle_request_digest({}, rodaq::OperationKind::Start, {}).ok());
    EXPECT_FALSE(rodaq::task::configure_request_digest({}, configuration, {}, {}).ok());
    configuration.pipelines[0].pipeline_id = 0;
    configuration.pipelines[0].streams[0].stream_id = 0;
    auto unassigned = rodaq::task::create_request_digest(configuration, {});
    ASSERT_TRUE(unassigned.ok());
    EXPECT_NE(unassigned.value(), created.value());
}

TEST(TaskRequest, ConfiguredLimitsArePreservedWithSeparateFixedEnvelopeBudget) {
    auto configuration = config();
    configuration.pipelines[0].acquisition.json = "{\"large\":\"" + std::string(70000, 'a') + "\"}";
    rodaq::TaskConfigLimits limits;
    limits.max_plugin_config_bytes = 80000;
    limits.max_json_string_bytes = 80000;
    auto exported = rodaq::export_task_config(configuration, limits);
    ASSERT_TRUE(exported.ok());
    limits.max_config_bytes = exported.value().size();
    limits.max_plugin_config_bytes = configuration.pipelines[0].acquisition.json.size();
    auto created = rodaq::task::create_request_digest(configuration, limits);
    ASSERT_TRUE(created.ok());
    const auto task = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
    EXPECT_TRUE(rodaq::task::configure_request_digest(task, configuration, {}, limits).ok());
    EXPECT_FALSE(rodaq::task::create_request_digest(configuration, {}).ok());
}

}  // namespace
