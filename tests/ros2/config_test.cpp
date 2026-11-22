#include "config.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace backend = rodaq::ros2_acquisition;
using Json = nlohmann::json;
using namespace std::chrono_literals;

std::string fixture(const char* name) {
    std::ifstream file(std::filesystem::path(RODAQ_ROS2_FIXTURES) / name);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

TEST(Ros2Config, GoldenDefaultsAndExplicitProfilesHaveTheSameTypedMeaningAcrossDistros) {
    auto minimal = backend::parse_config(fixture("minimal.json"));
    ASSERT_TRUE(minimal.ok());
    EXPECT_EQ(minimal.value().domain_id, 0);
    EXPECT_EQ(minimal.value().executor_threads, 2);
    EXPECT_EQ(minimal.value().graph_poll_interval, 500ms);
    EXPECT_EQ(minimal.value().qos_change_debounce, 500ms);
    ASSERT_EQ(minimal.value().streams.size(), 1);
    EXPECT_EQ(minimal.value().streams[0].key, "joint_states");
    EXPECT_EQ(minimal.value().streams[0].type, "sensor_msgs/msg/JointState");
    EXPECT_TRUE(minimal.value().streams[0].adapt);
    EXPECT_EQ(minimal.value().streams[0].requested.depth, 10);
    auto explicit_config = backend::parse_config(fixture("explicit.json"));
    ASSERT_TRUE(explicit_config.ok());
    EXPECT_EQ(explicit_config.value().domain_id, 173);
    EXPECT_EQ(explicit_config.value().graph_poll_interval, 100ms);
    EXPECT_EQ(explicit_config.value().qos_change_debounce, 50ms);
    ASSERT_EQ(explicit_config.value().streams.size(), 2);
    EXPECT_FALSE(explicit_config.value().streams[0].adapt);
    EXPECT_EQ(explicit_config.value().streams[0].requested.depth, 16);
    EXPECT_EQ(
        explicit_config.value().streams[0].requested.reliability,
        RODAQ_ROS2_QOS_RELIABILITY_RELIABLE);
    EXPECT_EQ(
        explicit_config.value().streams[0].requested.durability,
        RODAQ_ROS2_QOS_DURABILITY_TRANSIENT_LOCAL);
}

TEST(Ros2Config, DiscoveryMayOmitTypeButExplicitModeRequiresAllItsFields) {
    auto json = Json::parse(fixture("minimal.json"));
    json["streams"][0].erase("type");
    auto parsed = backend::parse_config(json.dump());
    ASSERT_TRUE(parsed.ok());
    EXPECT_FALSE(parsed.value().streams[0].type);
    json["streams"][0]["qos"]["mode"] = "explicit";
    EXPECT_FALSE(backend::parse_config(json.dump()).ok());
}

TEST(Ros2Config, UnknownFieldsDuplicateKeysDuplicateBindingsAndModeMixingAreRejected) {
    auto json = Json::parse(fixture("minimal.json"));
    json["unknown"] = true;
    EXPECT_FALSE(backend::parse_config(json.dump()).ok());
    json.erase("unknown");
    json["streams"][0]["qos"]["reliability"] = "reliable";
    EXPECT_FALSE(backend::parse_config(json.dump()).ok());
    json["streams"][0]["qos"].erase("reliability");
    json["streams"].push_back(json["streams"][0]);
    EXPECT_FALSE(backend::parse_config(json.dump()).ok());
    EXPECT_FALSE(
        backend::parse_config(R"({"format_version":"1.0","format_version":"1.0","streams":[]})")
            .ok());
}

TEST(Ros2Config, PublicRosTopicAndCanonicalTypeValidationRunBeforeCreatingAnyContext) {
    for (const auto* topic : {"relative", "/bad//name", "/1bad", "/", "/with space", "/a/"}) {
        auto json = Json::parse(fixture("minimal.json"));
        json["streams"][0]["topic"] = topic;
        EXPECT_FALSE(backend::parse_config(json.dump()).ok()) << topic;
    }
    auto json = Json::parse(fixture("minimal.json"));
    json["streams"][0]["topic"] = std::string("/good\0hidden", 12);
    EXPECT_FALSE(backend::parse_config(json.dump()).ok());
    json["streams"][0]["topic"] = "/good";
    json["streams"][0]["type"] = "../msg/Unsafe";
    EXPECT_FALSE(backend::parse_config(json.dump()).ok());
}

TEST(Ros2Config, IntegerDecimalDepthAndDurationBudgetsCannotOverflowOrSilentlyRound) {
    auto json = Json::parse(fixture("minimal.json"));
    json["domain_id"] = 233;
    EXPECT_FALSE(backend::parse_config(json.dump()).ok());
    json["domain_id"] = 1.5;
    EXPECT_FALSE(backend::parse_config(json.dump()).ok());
    json["domain_id"] = 232;
    EXPECT_TRUE(backend::parse_config(json.dump()).ok());
    for (const auto* depth : {"0", "01", "1000001", "18446744073709551616", "10\n"}) {
        json["streams"][0]["qos"]["depth"] = depth;
        EXPECT_FALSE(backend::parse_config(json.dump()).ok()) << depth;
    }
    json["streams"][0]["qos"].erase("depth");
    json["graph_poll_interval"] = {{"value", "0"}, {"unit", "ms"}};
    EXPECT_FALSE(backend::parse_config(json.dump()).ok());
    json["graph_poll_interval"] = {{"value", "60000000001"}, {"unit", "ns"}};
    EXPECT_FALSE(backend::parse_config(json.dump()).ok());
    json["graph_poll_interval"] = {{"value", "60"}, {"unit", "s"}};
    json["qos_change_debounce"] = {{"value", "0"}, {"unit", "ns"}};
    EXPECT_TRUE(backend::parse_config(json.dump()).ok());
}

}  // namespace
