#include "task/config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <fstream>
#include <future>
#include <vector>
#include <string>
#include <utility>

#include <rodaq/plugin/version.hpp>

namespace {

using rodaq::persistence::Json;
using namespace std::chrono_literals;

Json portable() {
    std::ifstream source(RODAQ_TASK_CONFIG_FIXTURE);
    return Json::parse(source);
}

TEST(TaskConfig, PortableRoundTripAndIndependentCanonicalIdentityGolden) {
    auto parsed = rodaq::parse_task_config(portable().dump());
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().segment_policy.max_duration, 60s);
    EXPECT_EQ(parsed.value().pipelines[0].pipeline_id, 10);
    EXPECT_EQ(parsed.value().pipelines[0].streams[0].stream_id, 20);
    auto identity = rodaq::task::config_version(parsed.value(), {});
    ASSERT_TRUE(identity.ok());
    EXPECT_EQ(
        identity.value(),
        "sha256:ab3a9a27a6183cfb5d025f5e58b6e489540f61f8b1f732acbed8fb61f717cfdc");
    auto exported = rodaq::export_task_config(parsed.value());
    ASSERT_TRUE(exported.ok());
    EXPECT_EQ(exported.value().find("\"name\""), std::string::npos);
    auto imported = rodaq::parse_task_config(exported.value());
    ASSERT_TRUE(imported.ok());
    EXPECT_EQ(rodaq::task::config_version(imported.value(), {}).value(), identity.value());
    auto reordered = portable();
    reordered["segment_policy"]["max_duration"] = {{"unit", "ms"}, {"value", "60000"}};
    auto equivalent = rodaq::parse_task_config(reordered.dump(4));
    ASSERT_TRUE(equivalent.ok());
    EXPECT_EQ(rodaq::task::config_version(equivalent.value(), {}).value(), identity.value());
}

TEST(TaskConfig, PreviousContinuousHistoryRequiresExplicitOptInAndFalsePreservesOldIdentity) {
    auto document = portable();
    const auto original = rodaq::parse_task_config(document.dump());
    ASSERT_TRUE(original.ok());
    const auto old_identity = rodaq::task::config_version(original.value(), {}).value();
    document["retention_policy"]["include_previous_continuous"] = false;
    auto explicit_false = rodaq::parse_task_config(document.dump());
    ASSERT_TRUE(explicit_false.ok());
    EXPECT_EQ(rodaq::task::config_version(explicit_false.value(), {}).value(), old_identity);
    EXPECT_EQ(
        rodaq::export_task_config(explicit_false.value()).value(),
        rodaq::export_task_config(original.value()).value());
    document["retention_policy"]["include_previous_continuous"] = true;
    EXPECT_FALSE(rodaq::parse_task_config(document.dump()).ok());
    document["recording_mode"] = "rolling";
    document["retention_policy"]["mode"] = "rolling";
    document["retention_policy"]["max_physical_bytes"] = "1000";
    auto enabled = rodaq::parse_task_config(document.dump());
    ASSERT_TRUE(enabled.ok());
    EXPECT_TRUE(enabled.value().retention_policy.include_previous_continuous);
    const auto exported = rodaq::export_task_config(enabled.value());
    ASSERT_TRUE(exported.ok());
    EXPECT_TRUE(Json::parse(exported.value())["retention_policy"]["include_previous_continuous"]);
    const auto restored = rodaq::parse_task_config(exported.value());
    ASSERT_TRUE(restored.ok());
    EXPECT_EQ(
        rodaq::task::config_version(restored.value(), {}).value(),
        rodaq::task::config_version(enabled.value(), {}).value());
    enabled.value().retention_policy.include_previous_continuous = false;
    EXPECT_NE(
        rodaq::task::config_version(enabled.value(), {}).value(),
        rodaq::task::config_version(restored.value(), {}).value());
    for (const auto& invalid : {Json(nullptr), Json(1), Json("true")}) {
        document["retention_policy"]["include_previous_continuous"] = invalid;
        EXPECT_FALSE(rodaq::parse_task_config(document.dump()).ok());
    }
}

TEST(TaskConfig, CreateMayOmitIdentitiesButConfigVersionRequiresIdCompleteModel) {
    auto value = portable();
    value["pipelines"][0].erase("pipeline_id");
    value["pipelines"][0]["streams"][0].erase("stream_id");
    auto parsed = rodaq::parse_task_config(value.dump());
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().pipelines[0].pipeline_id, 0);
    EXPECT_EQ(parsed.value().pipelines[0].streams[0].stream_id, 0);
    EXPECT_FALSE(rodaq::task::config_version(parsed.value(), {}).ok());
    EXPECT_TRUE(rodaq::export_task_config(parsed.value()).ok());
    value["pipelines"][0]["pipeline_id"] = "0";
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
}

TEST(TaskConfig, StreamIdentityIsUniqueAcrossTaskWhileBindingKeysArePipelineScoped) {
    auto value = portable();
    auto second = value["pipelines"][0];
    second["pipeline_id"] = "11";
    value["pipelines"].push_back(second);
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    value["pipelines"][1]["streams"][0]["stream_id"] = "21";
    EXPECT_TRUE(rodaq::parse_task_config(value.dump()).ok());
    value["pipelines"][0]["streams"].push_back(value["pipelines"][0]["streams"][0]);
    value["pipelines"][0]["streams"][1]["stream_id"] = "22";
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
}

TEST(TaskConfig, RequiredFieldsUnknownFieldsClosedEnumsAndNullableBoundsAreStrict) {
    for (const auto* field :
         {"recovery_policy", "start_policy", "segment_policy", "retention_policy",
          "format_version"}) {
        auto missing = portable();
        missing.erase(field);
        EXPECT_FALSE(rodaq::parse_task_config(missing.dump()).ok()) << field;
    }
    for (const auto& path :
         {Json::json_pointer("/extra"), Json::json_pointer("/start_policy/extra"),
          Json::json_pointer("/pipelines/0/extra"),
          Json::json_pointer("/pipelines/0/streams/0/extra")}) {
        auto extra = portable();
        extra[path] = true;
        EXPECT_FALSE(rodaq::parse_task_config(extra.dump()).ok());
    }
    auto value = portable();
    value["start_policy"]["mode"] = "future";
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    value = portable();
    value["segment_policy"]["max_duration"] = nullptr;
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    value["segment_policy"]["max_physical_bytes"] = "1";
    EXPECT_TRUE(rodaq::parse_task_config(value.dump()).ok());
    value["segment_policy"].erase("max_duration");
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
}

TEST(TaskConfig, RollingAndRecoveryPoliciesHaveOneSemanticAuthority) {
    auto value = portable();
    value["recording_mode"] = "rolling";
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    value["retention_policy"]["mode"] = "rolling";
    value["retention_policy"]["max_physical_bytes"] = "18446744073709551615";
    value["recovery_policy"]["mode"] = "resume";
    value["start_policy"]["mode"] = "best_effort";
    auto parsed = rodaq::parse_task_config(value.dump());
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().recording_mode, rodaq::RecordingMode::Rolling);
    EXPECT_EQ(parsed.value().recovery_policy, rodaq::RecoveryPolicyMode::Resume);
    EXPECT_EQ(
        parsed.value().retention_policy.max_physical_bytes,
        std::numeric_limits<std::uint64_t>::max());
}

TEST(TaskConfig, PluginPrivateJsonIsOpaqueButStrictAndVersionIdentityUsesFoundationRules) {
    auto value = portable();
    value["pipelines"][0]["acquisition"]["config"]["unknown_backend_field"] =
        Json::array({1, true, "opaque"});
    EXPECT_TRUE(rodaq::parse_task_config(value.dump()).ok());
    value["pipelines"][0]["acquisition"].erase("plugin_version");
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    for (const std::string version : {"01.0.0", "1.2", "1.0.0-01", "1.0.0+", "1.0.0+a+b"}) {
        value = portable();
        value["pipelines"][0]["acquisition"]["plugin_version"] = version;
        EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
        EXPECT_FALSE(rodaq::plugin::validate_plugin_version(version).ok());
    }
    value = portable();
    value["pipelines"][0]["acquisition"]["plugin_version"] =
        "999999999999999999999.0.0-rc.1+build.7";
    EXPECT_TRUE(rodaq::parse_task_config(value.dump()).ok());
    auto typed = rodaq::parse_task_config(portable().dump());
    ASSERT_TRUE(typed.ok());
    typed.value().pipelines[0].acquisition.json = R"({"x":1,"\u0078":2})";
    EXPECT_FALSE(rodaq::export_task_config(typed.value()).ok());
}

TEST(TaskConfig, DecimalStringsDurationOverflowUtf8ByteLimitsAndTypedEnumsFailClosed) {
    auto value = portable();
    value["pipelines"][0]["streams"][0]["stream_id"] = "18446744073709551615";
    EXPECT_TRUE(rodaq::parse_task_config(value.dump()).ok());
    for (const auto* decimal : {"00", "+1", "-1", "18446744073709551616"}) {
        value["pipelines"][0]["streams"][0]["stream_id"] = decimal;
        EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    }
    value = portable();
    value["segment_policy"]["max_duration"]["value"] = "9223372037";
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    value = portable();
    value["name"] = std::string(128, 'a');
    EXPECT_TRUE(rodaq::parse_task_config(value.dump()).ok());
    value["name"] = std::string(129, 'a');
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    value["name"] = "";
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    auto typed = rodaq::parse_task_config(portable().dump());
    ASSERT_TRUE(typed.ok());
    typed.value().start_policy = static_cast<rodaq::StartPolicyMode>(99);
    EXPECT_FALSE(rodaq::export_task_config(typed.value()).ok());
}

TEST(TaskConfig, CardinalityAndAggregateTypedResourceBudgetsApplyBeforeComposition) {
    rodaq::TaskConfigLimits limits;
    limits.max_pipelines = 1;
    auto value = portable();
    value["pipelines"].push_back(value["pipelines"][0]);
    EXPECT_FALSE(rodaq::parse_task_config(value.dump(), limits).ok());
    auto typed = rodaq::parse_task_config(portable().dump());
    ASSERT_TRUE(typed.ok());
    limits.max_json_nodes = 20;
    EXPECT_FALSE(rodaq::export_task_config(typed.value(), limits).ok());
    limits = {};
    limits.max_plugin_config_bytes = 2;
    EXPECT_FALSE(rodaq::parse_task_config(portable().dump(), limits).ok());
    limits = {};
    limits.max_config_bytes = 32;
    EXPECT_FALSE(rodaq::export_task_config(typed.value(), limits).ok());
}

TEST(TaskConfig, ConcurrentCallersKeepIndependentCanonicalIdentityAndOwnedConfiguration) {
    auto parsed = rodaq::parse_task_config(portable().dump());
    ASSERT_TRUE(parsed.ok());
    const auto config = std::move(parsed).value();
    const auto expected = rodaq::task::config_version(config, {});
    ASSERT_TRUE(expected.ok());
    std::vector<std::future<bool>> workers;
    for (unsigned int worker = 0; worker < 4; ++worker) {
        workers.push_back(std::async(std::launch::async, [&] {
            for (unsigned int attempt = 0; attempt < 50; ++attempt) {
                auto encoded = rodaq::export_task_config(config);
                if (!encoded.ok()) {
                    return false;
                }
                auto imported = rodaq::parse_task_config(encoded.value());
                if (!imported.ok()) {
                    return false;
                }
                auto identity = rodaq::task::config_version(imported.value(), {});
                if (!identity.ok() || identity.value() != expected.value()) {
                    return false;
                }
            }
            return true;
        }));
    }
    for (auto& worker : workers) {
        EXPECT_TRUE(worker.get());
    }
}

}  // namespace
