#include "test_support.hpp"

#include <rodaq/result.hpp>

#include <string>
#include <string_view>

#include "manifest.hpp"
#include "semver.hpp"

namespace {

constexpr std::string_view kValidManifest = R"json({
  "format_version": 1,
  "plugin": {
    "id": "com.rodaq.test.manifest",
    "name": "Manifest Test",
    "version": "1.2.3-rc.1+build.7",
    "abi": {
      "major": 1,
      "minor": 0
    }
  },
  "platform": {
    "os": "linux",
    "arch": "amd64"
  },
  "artifact": {
    "path": "lib/libmanifest_test.so",
    "sha256": "0000000000000000000000000000000000000000000000000000000000000000"
  }
})json";

}  // namespace

int main() {
    auto manifest = rodaq::plugin::parse_manifest(kValidManifest);
    RODAQ_TEST_CHECK(manifest.ok());
    RODAQ_TEST_CHECK(manifest.value().plugin_id == "com.rodaq.test.manifest");
    RODAQ_TEST_CHECK(manifest.value().plugin_version == "1.2.3-rc.1+build.7");

    auto duplicate = rodaq::plugin::parse_manifest(R"json({
      "format_version": 1,
      "format_version": 1,
      "plugin": {},
      "platform": {},
      "artifact": {}
    })json");
    RODAQ_TEST_CHECK(!duplicate.ok());

    auto unknown = rodaq::plugin::parse_manifest(R"json({
      "format_version": 1,
      "plugin": {
        "id": "com.rodaq.test.manifest",
        "name": "Manifest Test",
        "version": "1.0.0",
        "abi": {"major": 1, "minor": 0}
      },
      "platform": {"os": "linux", "arch": "amd64"},
      "artifact": {
        "path": "lib/test.so",
        "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
        "unknown": true
      }
    })json");
    RODAQ_TEST_CHECK(!unknown.ok());

    const auto replace_once = [](std::string_view input, std::string_view from,
                                 std::string_view to) {
        std::string output{input};
        const auto offset = output.find(from);
        if (offset != std::string::npos) {
            output.replace(offset, from.size(), to);
        }
        return output;
    };

    RODAQ_TEST_CHECK(
        !rodaq::plugin::parse_manifest(
             replace_once(kValidManifest, "\"format_version\": 1", "\"format_version\": 2"))
             .ok());
    RODAQ_TEST_CHECK(!rodaq::plugin::parse_manifest(
                          replace_once(kValidManifest, "lib/libmanifest_test.so", "../escape.so"))
                          .ok());
    RODAQ_TEST_CHECK(
        !rodaq::plugin::parse_manifest(
             replace_once(
                 kValidManifest, "0000000000000000000000000000000000000000000000000000000000000000",
                 "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"))
             .ok());

    auto release = rodaq::plugin::parse_semver("1.2.3");
    auto prerelease = rodaq::plugin::parse_semver("1.2.3-rc.1");
    auto build_a = rodaq::plugin::parse_semver("1.2.3+build.1");
    auto build_b = rodaq::plugin::parse_semver("1.2.3+build.2");
    RODAQ_TEST_CHECK(release.ok() && prerelease.ok());
    RODAQ_TEST_CHECK(
        rodaq::plugin::compare_semver_precedence(release.value(), prerelease.value()) > 0);
    RODAQ_TEST_CHECK(build_a.ok() && build_b.ok());
    RODAQ_TEST_CHECK(build_a.value().identity != build_b.value().identity);
    RODAQ_TEST_CHECK(
        rodaq::plugin::compare_semver_precedence(build_a.value(), build_b.value()) == 0);
    RODAQ_TEST_CHECK(!rodaq::plugin::parse_semver("1.02.3").ok());
    return 0;
}
