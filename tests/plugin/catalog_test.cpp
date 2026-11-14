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

plugin::CatalogEntry candidate(std::string version, std::string digest = "abc") {
    plugin::CatalogEntry entry;
    entry.info = {"com.example.counter", "Counter", std::move(version)};
    entry.binary = {entry.info.plugin_id, entry.info.plugin_version, std::move(digest)};
    entry.version = plugin::parse_version(entry.info.plugin_version).value();
    return entry;
}

TEST(PluginSemVer, StrictSyntaxAndUnboundedNumericMagnitudeWithinLengthLimit) {
    for (const std::string_view version :
         {"1.2", "01.2.3", "1.2.3-01", "1.2.3+", "1.2.3-", "1.2.3\n", "v1.2.3", "1.2.3+one+two",
          "1.2.3.4"}) {
        EXPECT_FALSE(plugin::parse_version(version).ok()) << version;
    }
    const auto large = plugin::parse_version("99999999999999999999999999.0.0");
    const auto small = plugin::parse_version("9999999999999999999999999.99.99");
    ASSERT_TRUE(large.ok());
    ASSERT_TRUE(small.ok());
    EXPECT_GT(plugin::compare_version(large.value(), small.value()), 0);
    const std::array versions{"1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                              "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0"};
    for (std::size_t i = 1; i < versions.size(); ++i) {
        const auto previous = plugin::parse_version(versions[i - 1]);
        const auto current = plugin::parse_version(versions[i]);
        ASSERT_TRUE(previous.ok());
        ASSERT_TRUE(current.ok());
        EXPECT_LT(plugin::compare_version(previous.value(), current.value()), 0);
    }
}

TEST(PluginResolver, BuildMetadataTieRequiresExactIdentity) {
    const std::vector entries{candidate("1.2.3+build.7"), candidate("1.2.3+build.8")};
    EXPECT_EQ(
        plugin::select_plugin(entries, "com.example.counter", {}).status().code(),
        rodaq::StatusCode::Conflict);
    EXPECT_EQ(plugin::select_plugin(entries, "com.example.counter", "1.2.3+build.8").value(), 1);
    const std::vector duplicates{candidate("1.2.3", "abc"), candidate("1.2.3", "different")};
    EXPECT_EQ(
        plugin::select_plugin(duplicates, "com.example.counter", "1.2.3").status().code(),
        rodaq::StatusCode::Conflict);
}

TEST(PluginResolver, PrereleasesAreOnlyExplicitAndStableSelectionIsHighest) {
    const std::vector entries{candidate("1.0.0"), candidate("10.0.0-rc.1"), candidate("2.0.0")};
    EXPECT_EQ(plugin::select_plugin(entries, "com.example.counter", {}).value(), 2);
    EXPECT_EQ(plugin::select_plugin(entries, "com.example.counter", "10.0.0-rc.1").value(), 1);
    const std::vector prerelease{candidate("1.0.0-rc.1")};
    EXPECT_EQ(
        plugin::select_plugin(prerelease, "com.example.counter", {}).status().code(),
        rodaq::StatusCode::Unavailable);
    EXPECT_EQ(
        plugin::select_plugin(entries, "unknown", {}).status().code(), rodaq::StatusCode::NotFound);
    EXPECT_EQ(
        plugin::select_plugin(entries, "../bad", {}).status().code(),
        rodaq::StatusCode::InvalidArgument);
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
        fs::permissions(root_, fs::perms::owner_all, error);
        for (fs::recursive_directory_iterator iterator(root_, error);
             iterator != fs::recursive_directory_iterator(); iterator.increment(error)) {
            fs::permissions(iterator->path(), fs::perms::owner_all, error);
        }
        fs::remove_all(root_, error);
    }

    static Json manifest() {
#if defined(__aarch64__)
        const auto* architecture = "arm64";
#else
        const auto* architecture = "amd64";
#endif
        return {
            {"format_version", 1},
            {"plugin",
             {{"id", "com.example.counter"},
              {"name", "Counter"},
              {"version", "1.0.0"},
              {"abi", {{"major", 1}, {"minor", 0}}}}},
            {"platform", {{"os", "linux"}, {"arch", architecture}}},
            {"artifact",
             {{"path", "lib/counter.so"},
              {"sha256", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}}}};
    }

    fs::path package(std::string_view name, std::string_view json) {
        const auto path = root_ / name;
        fs::create_directories(path / "lib");
        std::ofstream(path / "lib/counter.so", std::ios::binary) << "abc";
        std::ofstream(path / "manifest.json", std::ios::binary) << json;
        return path;
    }

    void make_readonly() {
        for (const auto& entry : fs::recursive_directory_iterator(root_)) {
            fs::permissions(
                entry.path(), fs::perms::owner_read | fs::perms::owner_exec |
                                  fs::perms::group_read | fs::perms::group_exec |
                                  fs::perms::others_read | fs::perms::others_exec);
        }
        fs::permissions(root_, fs::perms::owner_read | fs::perms::owner_exec);
    }

    fs::path root_;
};

TEST_F(PluginCatalogTest, ReadonlySnapshotAndArtifactIdentity) {
    package("valid", manifest().dump());
    make_readonly();
    const auto catalog = plugin::discover_plugins({root_});
    ASSERT_TRUE(catalog.ok());
    ASSERT_EQ(catalog.value().size(), 1);
    const auto& entry = catalog.value().front();
    EXPECT_EQ(entry.identity.size, 3);
    EXPECT_TRUE(plugin::verify_artifact(entry).ok());
    fs::permissions(entry.artifact, fs::perms::owner_all);
    EXPECT_EQ(plugin::verify_artifact(entry).status().code(), rodaq::StatusCode::PermissionDenied);
    std::ofstream(entry.artifact, std::ios::binary) << "abd";
    fs::permissions(entry.artifact, fs::perms::owner_read);
    EXPECT_EQ(plugin::verify_artifact(entry).status().code(), rodaq::StatusCode::DataLoss);
}

TEST_F(PluginCatalogTest, BadPackagesDoNotHideHealthyPackage) {
    package("valid", manifest().dump());
    auto unknown = manifest();
    unknown["unexpected"] = true;
    package("unknown", unknown.dump());
    auto wrong_type = manifest();
    wrong_type["format_version"] = 1.0;
    package("type", wrong_type.dump());
    auto duplicate = manifest().dump();
    duplicate.insert(1, "\"format_version\":1,");
    package("duplicate", duplicate);
    package("trailing-comma", "{\"format_version\":1,}");
    package("oversized", std::string(plugin::kMaxManifestSize + 1, ' '));
    make_readonly();
    const auto catalog = plugin::discover_plugins({root_});
    ASSERT_TRUE(catalog.ok());
    EXPECT_EQ(catalog.value().size(), 1);
}

TEST_F(PluginCatalogTest, LinkEscapeAndWritableAncestorsAreRejected) {
    package("healthy", manifest().dump());
    const auto escaping = package("escape", manifest().dump());
    std::ofstream(root_ / "outside.so") << "abc";
    fs::remove(escaping / "lib/counter.so");
    fs::create_symlink(root_ / "outside.so", escaping / "lib/counter.so");
    package("writable-parent/child", manifest().dump());
    make_readonly();
    fs::permissions(root_ / "writable-parent", fs::perms::owner_all);
    const auto catalog = plugin::discover_plugins({root_});
    ASSERT_TRUE(catalog.ok());
    ASSERT_EQ(catalog.value().size(), 1);
    EXPECT_EQ(catalog.value().front().root.filename(), "healthy");
}

TEST_F(PluginCatalogTest, WritableSearchRootIsUnsupported) {
    package("valid", manifest().dump());
    EXPECT_EQ(
        plugin::discover_plugins({root_}).status().code(), rodaq::StatusCode::PermissionDenied);
}

TEST_F(PluginCatalogTest, OversizedArtifactsAndPackagesDoNotConsumeUnboundedResources) {
    package("healthy", manifest().dump());
    const auto large = package("large", manifest().dump());
    fs::resize_file(large / "lib/counter.so", plugin::kMaxArtifactSize + 1);
    const auto many_files = package("many-files", manifest().dump());
    for (std::size_t i = 0; i < plugin::kMaxPackageFiles; ++i) {
        std::ofstream(many_files / std::to_string(i)) << "";
    }
    make_readonly();
    const auto catalog = plugin::discover_plugins({root_});
    ASSERT_TRUE(catalog.ok());
    ASSERT_EQ(catalog.value().size(), 1);
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
    make_readonly();
    EXPECT_EQ(
        plugin::discover_plugins({root_}).status().code(), rodaq::StatusCode::ResourceExhausted);
}

}  // namespace
