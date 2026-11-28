#include "task/config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include <rodaq/plugin/version.hpp>

namespace {

using rodaq::persistence::Json;
using namespace std::chrono_literals;

Json portable() {
    std::ifstream source(RODAQ_TASK_CONFIG_FIXTURE);
    return Json::parse(source);
}

TEST(TaskConfig, PortableRoundTripPreservesTypedFieldsAndEquality) {
    auto parsed = rodaq::parse_task_config(portable().dump());
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    EXPECT_EQ(parsed.value().segment_policy.max_duration, 60s);
    EXPECT_EQ(parsed.value().pipelines[0].pipeline_id, 10ULL);
    EXPECT_EQ(parsed.value().pipelines[0].streams[0].stream_id, 20ULL);

    auto exported = rodaq::export_task_config(parsed.value());
    ASSERT_TRUE(exported.ok());
    EXPECT_EQ(exported.value().find("\"name\""), std::string::npos);
    auto imported = rodaq::parse_task_config(exported.value());
    ASSERT_TRUE(imported.ok());
    EXPECT_TRUE(rodaq::task::equal_task_config(parsed.value(), imported.value()));
}

TEST(TaskConfig, CreateMayOmitIdentitiesButValidateWithRequireIdsEnforcesThem) {
    auto value = portable();
    value["pipelines"][0].erase("pipeline_id");
    value["pipelines"][0]["streams"][0].erase("stream_id");
    auto parsed = rodaq::parse_task_config(value.dump());
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().pipelines[0].pipeline_id, 0ULL);
    EXPECT_EQ(parsed.value().pipelines[0].streams[0].stream_id, 0ULL);
    EXPECT_FALSE(rodaq::task::validate_task_config(parsed.value(), {}, true).ok());
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

TEST(TaskConfig, RequiredFieldsObsoleteFieldsAndNullableBoundsAreStrict) {
    for (const auto* field :
         {"segment_policy", "retention_policy", "format_version", "pipelines"}) {
        auto missing = portable();
        missing.erase(field);
        EXPECT_FALSE(rodaq::parse_task_config(missing.dump()).ok()) << field;
    }
    for (const auto* obsolete : {"recording_mode", "start_policy", "recovery_policy"}) {
        auto legacy = portable();
        legacy[obsolete] = "legacy";
        EXPECT_FALSE(rodaq::parse_task_config(legacy.dump()).ok()) << obsolete;
    }
    auto value = portable();
    value["retention_policy"]["include_previous_continuous"] = false;
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());

    value = portable();
    value["segment_policy"]["max_duration_ms"] = nullptr;
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    value["segment_policy"]["max_physical_bytes"] = 1024;
    EXPECT_TRUE(rodaq::parse_task_config(value.dump()).ok());
}

TEST(TaskConfig, RollingRetentionRequiresAtLeastOneBoundWhileKeepAllForbidsBoth) {
    auto value = portable();
    value["retention_policy"]["mode"] = "rolling";
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    value["retention_policy"]["max_physical_bytes"] = 9007199254740991ULL;
    auto parsed = rodaq::parse_task_config(value.dump());
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().retention_policy.mode, rodaq::RetentionMode::Rolling);
    EXPECT_EQ(parsed.value().retention_policy.max_physical_bytes, 9007199254740991ULL);

    value["retention_policy"]["max_physical_bytes"] = 9007199254740992ULL;
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());

    value = portable();
    value["retention_policy"]["max_duration_ms"] = 1000;
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
}

TEST(TaskConfig, PluginPrivateJsonAndSemVerValidationFailClosed) {
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
    value["pipelines"][0]["acquisition"]["plugin_version"] = "1.2.3-rc.1+build.7";
    EXPECT_TRUE(rodaq::parse_task_config(value.dump()).ok());
}

TEST(TaskConfig, DecimalIdsUtf8ByteLimitsAndCardinalityLimitsApply) {
    auto value = portable();
    value["pipelines"][0]["streams"][0]["stream_id"] = "18446744073709551615";
    EXPECT_TRUE(rodaq::parse_task_config(value.dump()).ok());
    for (const auto* decimal : {"0", "00", "+1", "-1", "18446744073709551616"}) {
        value["pipelines"][0]["streams"][0]["stream_id"] = decimal;
        EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    }
    value = portable();
    value["name"] = std::string(128, 'a');
    EXPECT_TRUE(rodaq::parse_task_config(value.dump()).ok());
    value["name"] = std::string(129, 'a');
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());
    value["name"] = "";
    EXPECT_FALSE(rodaq::parse_task_config(value.dump()).ok());

    rodaq::TaskConfigLimits limits;
    limits.max_pipelines = 1;
    value = portable();
    value["pipelines"].push_back(value["pipelines"][0]);
    EXPECT_FALSE(rodaq::parse_task_config(value.dump(), limits).ok());
    limits = {};
    limits.max_plugin_config_bytes = 2;
    EXPECT_FALSE(rodaq::parse_task_config(portable().dump(), limits).ok());
    auto typed = rodaq::parse_task_config(portable().dump()).value();
    limits = {};
    limits.max_config_bytes = 32;
    EXPECT_FALSE(rodaq::export_task_config(typed, limits).ok());
}

TEST(TaskConfig, ConcurrentCallersExportAndParseIndependently) {
    auto parsed = rodaq::parse_task_config(portable().dump());
    ASSERT_TRUE(parsed.ok());
    const auto config = std::move(parsed).value();
    std::vector<std::future<bool>> workers;
    for (unsigned int worker = 0; worker < 4; ++worker) {
        workers.push_back(std::async(std::launch::async, [&] {
            for (unsigned int attempt = 0; attempt < 50; ++attempt) {
                auto encoded = rodaq::export_task_config(config);
                if (!encoded.ok()) {
                    return false;
                }
                auto imported = rodaq::parse_task_config(encoded.value());
                if (!imported.ok() || !rodaq::task::equal_task_config(imported.value(), config)) {
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
