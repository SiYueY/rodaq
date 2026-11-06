#include <rodaq/config/yaml_config_loader.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace rodaq {
namespace {

TEST(ConfigLoaderTest, ConvertsScalarsMapsAndListsWithoutYamlNodeLeakage) {
    const auto config = load_yaml_config(R"(
runtime:
  enabled: true
  limit: 42
  gain: 1.5
  name: acquisition
  values: [1, 2, 3]
)");
    ASSERT_TRUE(config.ok()) << config.status().message();
    const auto runtime = config.value().view().find("runtime");
    ASSERT_TRUE(runtime.ok());
    const auto enabled = runtime.value().find("enabled");
    ASSERT_TRUE(enabled.ok());
    const auto enabled_value = enabled.value().as_bool();
    ASSERT_TRUE(enabled_value.ok()) << enabled_value.status().message();
    EXPECT_TRUE(enabled_value.value());
    const auto limit = runtime.value().find("limit");
    ASSERT_TRUE(limit.ok());
    const auto limit_value = limit.value().as_int64();
    ASSERT_TRUE(limit_value.ok()) << limit_value.status().message();
    EXPECT_EQ(42, limit_value.value());
    const auto gain = runtime.value().find("gain");
    ASSERT_TRUE(gain.ok());
    const auto gain_value = gain.value().as_double();
    ASSERT_TRUE(gain_value.ok()) << gain_value.status().message();
    EXPECT_DOUBLE_EQ(1.5, gain_value.value());
    const auto name = runtime.value().find("name");
    ASSERT_TRUE(name.ok());
    const auto name_value = name.value().as_string();
    ASSERT_TRUE(name_value.ok()) << name_value.status().message();
    EXPECT_EQ("acquisition", name_value.value());
    const auto values = runtime.value().find("values");
    ASSERT_TRUE(values.ok());
    EXPECT_EQ(3U, values.value().size());
}

TEST(ConfigLoaderTest, RejectsDuplicateKeysAndFileSizeOverflow) {
    const auto duplicate = load_yaml_config("key: 1\nkey: 2\n");
    ASSERT_FALSE(duplicate.ok());
    EXPECT_EQ(StatusCode::Conflict, duplicate.status().status_code());

    YamlConfigLimits limits;
    limits.max_file_bytes = 4;
    const auto oversized = load_yaml_config("longer-than-four", limits);
    ASSERT_FALSE(oversized.ok());
    EXPECT_EQ(StatusCode::ResourceExhausted, oversized.status().status_code());
}

TEST(ConfigLoaderTest, EnforcesPostParseDepthLimit) {
    YamlConfigLimits limits;
    limits.max_depth = 1;
    const auto config = load_yaml_config("a:\n  b:\n    c: 1\n", limits);
    ASSERT_FALSE(config.ok());
    EXPECT_EQ(StatusCode::ResourceExhausted, config.status().status_code());
}

TEST(ConfigLoaderTest, EnforcesPostParseNodeCountLimit) {
    YamlConfigLimits limits;
    limits.max_nodes = 3;
    const auto config = load_yaml_config("root:\n  first: 1\n  second: 2\n", limits);
    ASSERT_FALSE(config.ok());
    EXPECT_EQ(StatusCode::ResourceExhausted, config.status().status_code());
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
    const auto config = load_yaml_config_file(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    ASSERT_TRUE(config.ok()) << config.status().message();
    ASSERT_TRUE(config.value().view().find("session").ok());
}

}  // namespace
}  // namespace rodaq
