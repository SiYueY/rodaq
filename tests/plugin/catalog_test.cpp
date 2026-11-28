#include "catalog.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace plugin = rodaq::plugin;
using Json = nlohmann::json;

plugin::CatalogEntry candidate(std::string id, std::string version) {
    plugin::CatalogEntry entry;
    entry.info = {std::move(id), "Counter", std::move(version)};
    entry.abi_major = 1;
    entry.abi_minor = 0;
    return entry;
}

TEST(PluginSemVer, StrictSyntaxValidation) {
    for (const std::string_view version :
         {"1.2", "01.2.3", "1.2.3-01", "1.2.3+", "1.2.3-", "1.2.3\n", "v1.2.3", "1.2.3+one+two",
          "1.2.3.4"}) {
        EXPECT_FALSE(plugin::parse_semver(version).ok()) << version;
    }
    for (const std::string_view version :
         {"0.0.0", "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-rc.1+build.001", "10.20.30"}) {
        EXPECT_TRUE(plugin::parse_semver(version).ok()) << version;
    }
}

TEST(PluginResolver, SingleVersionExactMatchAndConflictDetection) {
    const std::vector entries{candidate("com.example.counter", "1.2.3")};
    EXPECT_EQ(plugin::select_plugin(entries, "com.example.counter", std::nullopt).value(), 0U);
    EXPECT_EQ(plugin::select_plugin(entries, "com.example.counter", "1.2.3").value(), 0U);
    EXPECT_EQ(
        plugin::select_plugin(entries, "com.example.counter", "2.0.0").status().code(),
        rodaq::StatusCode::NotFound);
    EXPECT_EQ(
        plugin::select_plugin(entries, "unknown", std::nullopt).status().code(),
        rodaq::StatusCode::NotFound);
    EXPECT_EQ(
        plugin::select_plugin(entries, "../bad", std::nullopt).status().code(),
        rodaq::StatusCode::InvalidArgument);

    const std::vector duplicates{
        candidate("com.example.counter", "1.0.0"), candidate("com.example.counter", "2.0.0")};
    EXPECT_EQ(
        plugin::select_plugin(duplicates, "com.example.counter", "1.0.0").status().code(),
        rodaq::StatusCode::Conflict);
}

class PluginCatalogTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string value = "/tmp/rodaq-catalog-XXXXXX";
        std::copy(value.begin(), value.end(), pattern.begin());
        const auto* directory = mkdtemp(pattern.data());
        ASSERT_NE(directory, nullptr);
        root_ = directory;
    }

    void TearDown() override {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    static Json manifest(std::string id = "com.example.counter", std::string version = "1.0.0") {
#if defined(__aarch64__)
        const auto* architecture = "arm64";
#else
        const auto* architecture = "amd64";
#endif
        return {
            {"format_version", 1},
            {"plugin",
             {{"id", std::move(id)},
              {"name", "Counter"},
              {"version", std::move(version)},
              {"abi", {{"major", 1}, {"minor", 0}, {"revision", 1}}}}},
            {"platform", {{"os", "linux"}, {"arch", architecture}}},
            {"library", "lib/counter.so"}};
    }

    fs::path package(std::string_view name, std::string_view json) {
        const auto path = root_ / name;
        fs::create_directories(path / "lib");
        std::ofstream(path / "lib/counter.so", std::ios::binary) << "abc";
        std::ofstream(path / "manifest.json", std::ios::binary) << json;
        return path;
    }

    fs::path root_;
};

TEST_F(PluginCatalogTest, DiscoversValidPackage) {
    package("valid", manifest().dump());
    const auto catalog = plugin::discover_plugins({root_});
    ASSERT_TRUE(catalog.ok()) << catalog.status().message();
    ASSERT_EQ(catalog.value().size(), 1U);
    EXPECT_EQ(catalog.value().front().info.plugin_id, "com.example.counter");
    EXPECT_EQ(catalog.value().front().info.plugin_version, "1.0.0");
}

TEST_F(PluginCatalogTest, BadPackagesDoNotHideHealthyPackage) {
    package("valid", manifest().dump());
    auto unknown = manifest("com.example.bad1");
    unknown["unexpected"] = true;
    package("unknown", unknown.dump());
    auto wrong_type = manifest("com.example.bad2");
    wrong_type["format_version"] = 1.0;
    package("type", wrong_type.dump());
    package("trailing-comma", "{\"format_version\":1,}");
    package("oversized", std::string(plugin::kMaxManifestSize + 1, ' '));
    const auto catalog = plugin::discover_plugins({root_});
    ASSERT_TRUE(catalog.ok());
    ASSERT_EQ(catalog.value().size(), 1U);
    EXPECT_EQ(catalog.value().front().info.plugin_id, "com.example.counter");
}

TEST_F(PluginCatalogTest, SymlinkEscapeIsRejected) {
    package("healthy", manifest().dump());
    const auto escaping = package("escape", manifest("com.example.escape").dump());
    std::ofstream(root_ / "outside.so") << "abc";
    fs::remove(escaping / "lib/counter.so");
    fs::create_symlink(root_ / "outside.so", escaping / "lib/counter.so");
    const auto catalog = plugin::discover_plugins({root_});
    ASSERT_TRUE(catalog.ok());
    ASSERT_EQ(catalog.value().size(), 1U);
    EXPECT_EQ(catalog.value().front().root.filename(), "healthy");
}

TEST_F(PluginCatalogTest, PackageAndGlobalLimitsAreExecutable) {
    std::vector<fs::path> roots(plugin::kMaxPluginRoots + 1, root_);
    EXPECT_EQ(
        plugin::discover_plugins(roots).status().code(), rodaq::StatusCode::ResourceExhausted);
    for (std::size_t i = 0; i <= plugin::kMaxPackageEntries; ++i) {
        const auto path = root_ / std::to_string(i);
        fs::create_directory(path);
        std::ofstream(path / "manifest.json") << "{}";
    }
    EXPECT_EQ(
        plugin::discover_plugins({root_}).status().code(), rodaq::StatusCode::ResourceExhausted);
}

TEST_F(PluginCatalogTest, RejectsMissingOrWrongAbiRevisionAndDuplicateKeys) {
    package("valid", manifest("com.example.valid").dump());

    auto missing_rev = manifest("com.example.norev");
    missing_rev["plugin"]["abi"].erase("revision");
    package("missing_rev", missing_rev.dump());

    auto wrong_rev = manifest("com.example.wrongrev");
    wrong_rev["plugin"]["abi"]["revision"] = 2;
    package("wrong_rev", wrong_rev.dump());

    auto zero_rev = manifest("com.example.zerorev");
    zero_rev["plugin"]["abi"]["revision"] = 0;
    package("zero_rev", zero_rev.dump());

    // Duplicate key in plugin.manifest.json must fail closed.
    const std::string dup = R"({
        "format_version": 1,
        "plugin": {
            "id": "com.example.dup",
            "id": "com.example.dup",
            "name": "Dup",
            "version": "1.0.0",
            "abi": {"major": 1, "minor": 0, "revision": 1}
        },
        "platform": {"os": "linux", "arch": "amd64"},
        "library": "lib/counter.so"
    })";
    package("dup_key", dup);

    const auto catalog = plugin::discover_plugins({root_});
    ASSERT_TRUE(catalog.ok()) << catalog.status().message();
    ASSERT_EQ(catalog.value().size(), 1U);
    EXPECT_EQ(catalog.value().front().info.plugin_id, "com.example.valid");
}

}  // namespace
