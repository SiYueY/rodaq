#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "catalog.hpp"
#include "loader.hpp"

int main() {
    const std::filesystem::path plugin_root{RODAQ_TEST_PLUGIN_ROOT};
    auto catalog = rodaq::plugin::discover_catalog({plugin_root});
    RODAQ_TEST_CHECK(catalog.ok());
    RODAQ_TEST_CHECK(catalog.value().diagnostics.empty());
    rodaq::plugin::CatalogLimits root_limit;
    root_limit.max_roots = 0u;
    RODAQ_TEST_CHECK(!rodaq::plugin::discover_catalog({plugin_root}, root_limit).ok());

    rodaq::plugin::CatalogLimits file_limit;
    file_limit.max_package_files = 0u;
    auto limited = rodaq::plugin::discover_catalog({plugin_root}, file_limit);
    RODAQ_TEST_CHECK(limited.ok());
    RODAQ_TEST_CHECK(limited.value().entries.empty());
    RODAQ_TEST_CHECK(!limited.value().diagnostics.empty());

    rodaq::plugin::CatalogLimits path_limit;
    path_limit.max_path_length = 1u;
    auto limited_paths = rodaq::plugin::discover_catalog({plugin_root}, path_limit);
    RODAQ_TEST_CHECK(limited_paths.ok());
    RODAQ_TEST_CHECK(limited_paths.value().entries.empty());

    const rodaq::plugin::CatalogEntry* selected = nullptr;
    const rodaq::plugin::CatalogEntry* missing_entry = nullptr;
    for (const auto& candidate : catalog.value().entries) {
        if (candidate.manifest.plugin_id == "com.rodaq.test.counter") {
            selected = &candidate;
        } else if (candidate.manifest.plugin_id == "com.rodaq.test.missing-entry-provenance") {
            missing_entry = &candidate;
        }
    }
    RODAQ_TEST_CHECK(selected != nullptr);
    RODAQ_TEST_CHECK(missing_entry != nullptr);
    const auto& entry = *selected;
    RODAQ_TEST_CHECK(entry.manifest.plugin_id == "com.rodaq.test.counter");
    RODAQ_TEST_CHECK(entry.manifest.plugin_version == "1.0.0");

    auto library = rodaq::plugin::load_plugin_library(entry);
    RODAQ_TEST_CHECK(library.ok());
    RODAQ_TEST_CHECK(library.value()->info().plugin_id == entry.manifest.plugin_id);
    RODAQ_TEST_CHECK(library.value()->artifact_info().sha256 == entry.manifest.artifact_sha256);
    RODAQ_TEST_CHECK(library.value()->phase() == rodaq::plugin::PluginLibraryPhase::Loaded);
    auto missing_entry_library = rodaq::plugin::load_plugin_library(*missing_entry);
    RODAQ_TEST_CHECK(!missing_entry_library.ok());
    RODAQ_TEST_CHECK(missing_entry_library.status().code() == rodaq::StatusCode::PermissionDenied);

    const auto tampered_root = std::filesystem::temp_directory_path() / "rodaq-tampered-catalog";
    std::error_code error;
    std::filesystem::remove_all(tampered_root, error);
    std::filesystem::create_directories(tampered_root);
    std::filesystem::copy(
        std::filesystem::path{RODAQ_TEST_COUNTER_PACKAGE}, tampered_root / "counter",
        std::filesystem::copy_options::recursive);
    const auto tampered_artifact =
        tampered_root / "counter" / "lib" / "libplugin_counter_fixture.so";
    std::fstream artifact{tampered_artifact, std::ios::in | std::ios::out | std::ios::binary};
    RODAQ_TEST_CHECK(artifact.is_open());
    artifact.seekp(-1, std::ios::end);
    artifact.put('\x7f');
    artifact.close();

    auto tampered = rodaq::plugin::discover_catalog({tampered_root});
    RODAQ_TEST_CHECK(tampered.ok());
    RODAQ_TEST_CHECK(tampered.value().entries.empty());
    RODAQ_TEST_CHECK(!tampered.value().diagnostics.empty());
    std::filesystem::remove_all(tampered_root, error);
    return 0;
}
