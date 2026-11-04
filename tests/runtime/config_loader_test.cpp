#include <rodaq/config/yaml_config_loader.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace rodaq {
namespace {

TEST(ConfigLoaderTest, ConvertsScalarsMapsAndListsWithoutYamlNodeLeakage) {
    const auto config = LoadYamlConfig(R"(
runtime:
  enabled: true
  limit: 42
  gain: 1.5
  name: acquisition
  values: [1, 2, 3]
)");
    ASSERT_TRUE(config.has_value()) << config.error().message();
    const auto runtime = config.value().view().find("runtime");
    ASSERT_TRUE(runtime.has_value());
    const auto enabled = runtime.value().find("enabled");
    ASSERT_TRUE(enabled.has_value());
    const auto enabled_value = enabled.value().as_bool();
    ASSERT_TRUE(enabled_value.has_value()) << enabled_value.error().message();
    EXPECT_TRUE(enabled_value.value());
    const auto limit = runtime.value().find("limit");
    ASSERT_TRUE(limit.has_value());
    const auto limit_value = limit.value().as_int64();
    ASSERT_TRUE(limit_value.has_value()) << limit_value.error().message();
    EXPECT_EQ(42, limit_value.value());
    const auto gain = runtime.value().find("gain");
    ASSERT_TRUE(gain.has_value());
    const auto gain_value = gain.value().as_double();
    ASSERT_TRUE(gain_value.has_value()) << gain_value.error().message();
    EXPECT_DOUBLE_EQ(1.5, gain_value.value());
    const auto name = runtime.value().find("name");
    ASSERT_TRUE(name.has_value());
    const auto name_value = name.value().as_string();
    ASSERT_TRUE(name_value.has_value()) << name_value.error().message();
    EXPECT_EQ("acquisition", name_value.value());
    const auto values = runtime.value().find("values");
    ASSERT_TRUE(values.has_value());
    EXPECT_EQ(3U, values.value().size());
}

TEST(ConfigLoaderTest, RejectsDuplicateKeysAndFileSizeOverflow) {
    const auto duplicate = LoadYamlConfig("key: 1\nkey: 2\n");
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(ErrorCode::Conflict, duplicate.error().code());

    YamlConfigLimits limits;
    limits.max_file_bytes = 4;
    const auto oversized = LoadYamlConfig("longer-than-four", limits);
    ASSERT_FALSE(oversized.has_value());
    EXPECT_EQ(ErrorCode::ResourceExhausted, oversized.error().code());
}

TEST(ConfigLoaderTest, EnforcesPostParseDepthLimit) {
    YamlConfigLimits limits;
    limits.max_depth = 1;
    const auto config = LoadYamlConfig("a:\n  b:\n    c: 1\n", limits);
    ASSERT_FALSE(config.has_value());
    EXPECT_EQ(ErrorCode::ResourceExhausted, config.error().code());
}

TEST(ConfigLoaderTest, EnforcesPostParseNodeCountLimit) {
    YamlConfigLimits limits;
    limits.max_nodes = 3;
    const auto config = LoadYamlConfig("root:\n  first: 1\n  second: 2\n", limits);
    ASSERT_FALSE(config.has_value());
    EXPECT_EQ(ErrorCode::ResourceExhausted, config.error().code());
}

TEST(ConfigLoaderTest, LoadsFromFile) {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("rodaq-config-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".yaml");
    {
        std::ofstream output(path);
        output << "session: test\n";
    }
    const auto config = LoadYamlConfigFile(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    ASSERT_TRUE(config.has_value()) << config.error().message();
    ASSERT_TRUE(config.value().view().find("session").has_value());
}

}  // namespace
}  // namespace rodaq
