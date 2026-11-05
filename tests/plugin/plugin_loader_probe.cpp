#include "plugin/acquisition_plugin_instance.hpp"
#include "plugin/plugin_library.hpp"
#include "plugin/plugin_manager.hpp"
#include "plugin/shared_library.hpp"
#include "plugin/storage_plugin_instance.hpp"

#include <rodaq/error.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using rodaq::ErrorCode;
using rodaq::Result;
using rodaq::plugin::AcquisitionPluginInstance;
using rodaq::plugin::PluginCatalog;
using rodaq::plugin::PluginFamily;
using rodaq::plugin::PluginInstanceLease;
using rodaq::plugin::PluginLibrary;
using rodaq::plugin::PluginLibraryLease;
using rodaq::plugin::PluginManager;
using rodaq::plugin::SharedLibrary;
using rodaq::plugin::StoragePluginInstance;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::vector<std::string> read_events(const fs::path& path) {
    std::ifstream input(path);
    std::vector<std::string> events;
    std::string line;
    while (std::getline(input, line)) {
        events.push_back(std::move(line));
    }
    return events;
}

std::size_t count_event(const std::vector<std::string>& events, std::string_view name) {
    std::size_t count = 0;
    for (const std::string& event : events) {
        if (event == name) {
            ++count;
        }
    }
    return count;
}

bool has_event(const std::vector<std::string>& events, std::string_view name) {
    return count_event(events, name) != 0;
}

void expect_error(
    const Result<PluginLibrary>& result, int expected_code, std::string_view scenario) {
    require(!result.has_value(), scenario);
    require(
        static_cast<int>(result.error().code()) == expected_code,
        "Unexpected Plugin error category");
}

void configure_event_log(const fs::path& path) {
    std::error_code error;
    fs::remove(path, error);
    setenv("RODAQ_TEST_EVENT_LOG", path.c_str(), 1);
}

void check_shared_library(
    const fs::path& valid_path, const fs::path& plain_path, const fs::path& non_dso_path,
    const fs::path& missing_path, const fs::path& event_path) {
    configure_event_log(event_path);

    Result<SharedLibrary> missing = SharedLibrary::open(missing_path);
    require(!missing.has_value(), "Missing SharedLibrary path was accepted");
    require(missing.error().code() == ErrorCode::NotFound, "Missing path error category");

    Result<SharedLibrary> non_dso = SharedLibrary::open(non_dso_path);
    require(!non_dso.has_value(), "Non-DSO file was accepted");
    require(non_dso.error().code() == ErrorCode::Unavailable, "Non-DSO error category");

    Result<SharedLibrary> plain = SharedLibrary::open(plain_path);
    require(plain.has_value(), "Plain DSO failed to open");
    Result<void*> absent = plain.value().find_symbol("missing_symbol");
    require(!absent.has_value(), "Missing symbol was accepted");
    require(absent.error().code() == ErrorCode::NotFound, "Missing symbol error category");

    Result<SharedLibrary> opened = SharedLibrary::open(valid_path);
    require(opened.has_value(), "Valid DSO failed to open");
    require(opened.value().valid(), "Valid DSO is not valid");

    SharedLibrary moved(std::move(opened.value()));
    require(moved.valid(), "Moved SharedLibrary lost ownership");
    require(!opened.value().valid(), "Moved-from SharedLibrary remains valid");

    SharedLibrary assigned = std::move(moved);
    require(assigned.valid(), "Move assignment lost SharedLibrary ownership");
    // Verify the specified empty moved-from state.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    require(!moved.valid(), "Move-assigned-from SharedLibrary remains valid");
    Result<void*> moved_from_symbol = moved.find_symbol("rodaq_plugin_abi_version");
    require(!moved_from_symbol.has_value(), "Moved-from SharedLibrary resolved a symbol");
    require(
        moved_from_symbol.error().code() == ErrorCode::InvalidState,
        "Moved-from SharedLibrary error category");

    require(!has_event(read_events(event_path), "unload"), "DSO unloaded while owned");
    Result<SharedLibrary> replacement = SharedLibrary::open(plain_path);
    require(replacement.has_value(), "Replacement DSO failed to open");
    assigned = std::move(replacement.value());
    require(
        has_event(read_events(event_path), "unload"), "Final SharedLibrary release did not unload");
}

void validate_success(const std::vector<fs::path>& paths) {
    std::size_t index = 0;
    for (const fs::path& path : paths) {
        Result<PluginLibrary> library = PluginLibrary::open(path);
        require(library.has_value(), "Valid Plugin library was rejected");
        require(library.value().valid(), "Valid Plugin library is inert");
        require(!library.value().metadata().id.empty(), "Valid Plugin ID is empty");
        require(library.value().lease().raw_descriptor() != nullptr, "Valid descriptor is null");
        if (index == 0) {
            require(
                library.value().family() == PluginFamily::Acquisition,
                "Acquisition family was not preserved");
        } else if (index == 1) {
            require(
                library.value().family() == PluginFamily::Storage,
                "Storage family was not preserved");
        }
        ++index;
    }
}

void validate_error(int expected_code, const fs::path& path) {
    expect_error(PluginLibrary::open(path), expected_code, "Invalid Plugin library was accepted");
}

void validate_error_without_entrypoint(
    int expected_code, const fs::path& path, const fs::path& event_path) {
    configure_event_log(event_path);
    expect_error(PluginLibrary::open(path), expected_code, "Invalid Plugin library was accepted");
    require(
        !has_event(read_events(event_path), "entrypoint"),
        "Family entrypoint executed before required validation");
}

void catalog_conflict(const fs::path& first_path, const fs::path& second_path) {
    PluginManager manager;
    Result<PluginLibraryLease> first = manager.load_library(first_path);
    require(first.has_value(), "First Plugin failed to load");
    Result<PluginLibraryLease> second = manager.load_library(second_path);
    require(!second.has_value(), "Duplicate Plugin ID was accepted");
    require(second.error().code() == ErrorCode::Conflict, "Duplicate Plugin ID error category");
}

void family_mismatch(
    const fs::path& acquisition_path, const fs::path& storage_path, const fs::path& event_path) {
    configure_event_log(event_path);
    PluginManager manager;
    Result<PluginLibraryLease> acquisition = manager.load_library(acquisition_path);
    Result<PluginLibraryLease> storage = manager.load_library(storage_path);
    require(
        acquisition.has_value() && storage.has_value(), "Family mismatch fixtures failed to load");

    Result<StoragePluginInstance> storage_instance =
        manager.create_storage_plugin_instance(std::move(acquisition.value()));
    require(!storage_instance.has_value(), "Acquisition lease created a Storage instance");
    require(
        storage_instance.error().code() == ErrorCode::InvalidArgument,
        "Acquisition-to-Storage mismatch error category");

    Result<AcquisitionPluginInstance> acquisition_instance =
        manager.create_acquisition_plugin_instance(std::move(storage.value()));
    require(!acquisition_instance.has_value(), "Storage lease created an Acquisition instance");
    require(
        acquisition_instance.error().code() == ErrorCode::InvalidArgument,
        "Storage-to-Acquisition mismatch error category");
    require(
        !has_event(read_events(event_path), "create"), "Family mismatch called Plugin create()");
}

void catalog_path_merge(const fs::path& path, const fs::path& event_path) {
    configure_event_log(event_path);
    PluginManager manager;
    Result<PluginLibraryLease> first = manager.load_library(path);
    Result<PluginLibraryLease> second = manager.load_library(path);
    require(first.has_value() && second.has_value(), "Same-path Plugin load failed");
    require(
        count_event(read_events(event_path), "load") == 1,
        "Same canonical path was opened more than once");

    Result<void> unloaded = manager.unload(first.value().metadata().id);
    require(unloaded.has_value(), "Plugin unload request failed");
    require(!has_event(read_events(event_path), "unload"), "DSO unloaded while leases existed");

    first.value() = PluginLibraryLease{};
    require(!has_event(read_events(event_path), "unload"), "DSO unloaded before final lease");
    second.value() = PluginLibraryLease{};
    require(has_event(read_events(event_path), "unload"), "Final lease did not unload DSO");
}

void library_lease(const fs::path& path, const fs::path& event_path) {
    configure_event_log(event_path);
    PluginLibraryLease lease;
    {
        PluginManager manager;
        Result<PluginLibraryLease> loaded = manager.load_library(path);
        require(loaded.has_value(), "Plugin load failed");
        lease = loaded.value();
    }
    require(
        !has_event(read_events(event_path), "unload"), "DSO unloaded while library lease existed");
    lease = PluginLibraryLease{};
    require(has_event(read_events(event_path), "unload"), "Final library lease did not unload DSO");
}

void catalog_remove_with_live_instance(const fs::path& path, const fs::path& event_path) {
    configure_event_log(event_path);
    PluginManager manager;
    Result<PluginLibraryLease> library = manager.load_library(path);
    require(library.has_value(), "Plugin load failed");
    const std::string plugin_id = library.value().metadata().id;

    PluginInstanceLease instance_lease;
    {
        Result<AcquisitionPluginInstance> instance =
            manager.create_acquisition_plugin_instance(std::move(library.value()));
        require(instance.has_value(), "Acquisition create failed");
        instance_lease = instance.value().lease();

        Result<void> unloaded = manager.unload(plugin_id);
        require(unloaded.has_value(), "Plugin unload request failed");
        Result<PluginLibraryLease> found = manager.find(plugin_id);
        require(!found.has_value(), "Catalog entry remained visible");
        require(found.error().code() == ErrorCode::NotFound, "Catalog removal error category");

        std::vector<std::string> removed_events = read_events(event_path);
        require(!has_event(removed_events, "destroy"), "Catalog removal destroyed instance");
        require(!has_event(removed_events, "unload"), "Catalog removal unloaded DSO");
        instance.value().destroy();
    }

    require(
        count_event(read_events(event_path), "destroy") == 1,
        "Explicit instance destroy was not recorded exactly once");
    require(
        !has_event(read_events(event_path), "unload"),
        "DSO unloaded while lease remained after instance destroy");
    instance_lease = PluginInstanceLease{};

    std::vector<std::string> released_events = read_events(event_path);
    require(has_event(released_events, "unload"), "Final instance release did not unload DSO");
}

void catalog_remove_with_live_library(const fs::path& path, const fs::path& event_path) {
    configure_event_log(event_path);
    PluginManager manager;
    Result<PluginLibraryLease> loaded = manager.load_library(path);
    require(loaded.has_value(), "Plugin load failed");
    const std::string plugin_id = loaded.value().metadata().id;
    PluginLibraryLease retained = loaded.value();
    const rodaq_plugin_descriptor* descriptor = retained.raw_descriptor();
    const rodaq_acquisition_plugin* protocol = retained.acquisition_protocol();
    require(descriptor != nullptr && protocol != nullptr, "Library metadata is unavailable");

    Result<void> unloaded = manager.unload(plugin_id);
    require(unloaded.has_value(), "Plugin unload request failed");
    Result<PluginLibraryLease> found = manager.find(plugin_id);
    require(!found.has_value(), "Catalog entry remained visible");
    require(found.error().code() == ErrorCode::NotFound, "Catalog removal error category");
    require(descriptor->id.size > 0, "Descriptor became unreadable after catalog removal");
    require(protocol->query_interface != nullptr, "Protocol became invalid after catalog removal");
    require(
        !has_event(read_events(event_path), "unload"),
        "DSO unloaded while library leases remained");

    loaded.value() = PluginLibraryLease{};
    require(
        !has_event(read_events(event_path), "unload"), "DSO unloaded before final library lease");
    retained = PluginLibraryLease{};
    require(has_event(read_events(event_path), "unload"), "Final library lease did not unload DSO");
}

template <typename PluginInstance>
void plugin_instance_lease(const fs::path& path, bool acquisition, const fs::path& event_path) {
    configure_event_log(event_path);
    PluginInstanceLease instance_lease;
    {
        PluginManager manager;
        Result<PluginLibraryLease> library = manager.load_library(path);
        require(library.has_value(), "Plugin load failed");
        const std::string plugin_id = library.value().metadata().id;
        PluginInstanceLease lease;
        if (acquisition) {
            Result<AcquisitionPluginInstance> instance =
                manager.create_acquisition_plugin_instance(std::move(library.value()));
            require(instance.has_value(), "Acquisition create failed");
            lease = instance.value().lease();
            instance.value().destroy();
        } else {
            Result<StoragePluginInstance> instance =
                manager.create_storage_plugin_instance(std::move(library.value()));
            require(instance.has_value(), "Storage create failed");
            lease = instance.value().lease();
            instance.value().destroy();
        }
        instance_lease = lease;
        Result<void> unloaded = manager.unload(plugin_id);
        require(unloaded.has_value(), "Plugin unload request failed");
    }

    std::vector<std::string> before_destroy = read_events(event_path);
    require(
        count_event(before_destroy, "destroy") == 1,
        "Explicit instance destroy was not recorded exactly once");
    require(!has_event(before_destroy, "unload"), "DSO unloaded while instance lease existed");

    instance_lease = PluginInstanceLease{};
    std::vector<std::string> after_destroy = read_events(event_path);
    require(has_event(after_destroy, "unload"), "Instance did not transitively hold library");
}

void descriptor_lifetime(const fs::path& path, const fs::path& event_path) {
    configure_event_log(event_path);
    PluginLibraryLease lease;
    {
        PluginManager manager;
        Result<PluginLibraryLease> loaded = manager.load_library(path);
        require(loaded.has_value(), "Plugin load failed");
        lease = loaded.value();
    }

    const rodaq_plugin_descriptor* descriptor = lease.raw_descriptor();
    require(descriptor != nullptr, "Descriptor pointer is null");
    require(descriptor->id.size > 0, "Borrowed descriptor is not readable");
    require(!has_event(read_events(event_path), "unload"), "DSO unloaded under descriptor lease");
    lease = PluginLibraryLease{};
    require(has_event(read_events(event_path), "unload"), "Descriptor lease did not release DSO");
}

void create_failure(const fs::path& path) {
    PluginManager manager;
    Result<PluginLibraryLease> library = manager.load_library(path);
    require(library.has_value(), "Create-failure fixture failed to load");
    Result<AcquisitionPluginInstance> instance =
        manager.create_acquisition_plugin_instance(std::move(library.value()));
    require(!instance.has_value(), "Fixture create failure was accepted");
    require(instance.error().code() == ErrorCode::Unavailable, "Create error category");
    require(instance.error().native_code() == 77, "Create native code");
    require(instance.error().domain() == "fixture", "Create error domain");
    require(instance.error().message() == "fixture create failure", "Create error message");
}

void create_null(const fs::path& path) {
    PluginManager manager;
    Result<PluginLibraryLease> library = manager.load_library(path);
    require(library.has_value(), "Create-null fixture failed to load");
    Result<AcquisitionPluginInstance> instance =
        manager.create_acquisition_plugin_instance(std::move(library.value()));
    require(!instance.has_value(), "OK + null create instance was accepted");
    require(instance.error().code() == ErrorCode::InvalidArgument, "Null create error category");
}

void quarantine(const fs::path& path, const fs::path& event_path) {
    configure_event_log(event_path);
    PluginInstanceLease lease;
    {
        PluginManager manager;
        Result<PluginLibraryLease> library = manager.load_library(path);
        require(library.has_value(), "Quarantine fixture failed to load");
        const std::string plugin_id = library.value().metadata().id;
        PluginLibraryLease retained_library = library.value();
        Result<AcquisitionPluginInstance> instance =
            manager.create_acquisition_plugin_instance(std::move(library.value()));
        require(instance.has_value(), "Quarantine fixture create failed");
        lease = instance.value().lease();
        manager.quarantine(lease);

        Result<PluginLibraryLease> found = manager.find(plugin_id);
        require(!found.has_value(), "Quarantined Plugin remained available in catalog");
        require(
            found.error().code() == ErrorCode::Unavailable, "Quarantine catalog error category");

        Result<AcquisitionPluginInstance> blocked =
            manager.create_acquisition_plugin_instance(std::move(retained_library));
        require(!blocked.has_value(), "Quarantined Plugin created another instance");
        require(
            blocked.error().code() == ErrorCode::Unavailable, "Quarantine create error category");
    }
    lease = PluginInstanceLease{};

    std::vector<std::string> events = read_events(event_path);
    require(!has_event(events, "destroy"), "Quarantined instance was destroyed");
    require(!has_event(events, "unload"), "Quarantined DSO was unloaded before process exit");
}

void provenance_error(const fs::path& path, const fs::path& event_path) {
    configure_event_log(event_path);
    expect_error(
        PluginLibrary::open(path), static_cast<int>(ErrorCode::InvalidArgument),
        "Dependency-provided Plugin symbol was accepted");
    require(
        !has_event(read_events(event_path), "entrypoint"),
        "Unproven family entrypoint was executed");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "Missing probe scenario");
        const std::string_view scenario(argv[1]);

        if (scenario == "shared_library") {
            require(argc == 7, "shared_library expects five paths");
            check_shared_library(argv[3], argv[4], argv[5], argv[6], argv[2]);
        } else if (scenario == "validate_success") {
            std::vector<fs::path> paths;
            for (int index = 3; index < argc; ++index) {
                paths.emplace_back(argv[index]);
            }
            validate_success(paths);
        } else if (scenario == "validate_error") {
            require(argc == 4, "validate_error expects code and path");
            validate_error(std::stoi(argv[2]), argv[3]);
        } else if (scenario == "validate_error_without_entrypoint") {
            require(argc == 5, "validate_error_without_entrypoint expects code, path, event path");
            validate_error_without_entrypoint(std::stoi(argv[2]), argv[3], argv[4]);
        } else if (scenario == "catalog_conflict") {
            require(argc == 4, "catalog_conflict expects two paths");
            catalog_conflict(argv[2], argv[3]);
        } else if (scenario == "family_mismatch") {
            require(argc == 5, "family_mismatch expects two paths and an event path");
            family_mismatch(argv[2], argv[3], argv[4]);
        } else if (scenario == "catalog_path_merge") {
            require(argc == 4, "catalog_path_merge expects path and event path");
            catalog_path_merge(argv[2], argv[3]);
        } else if (scenario == "catalog_remove_instance_lifetime") {
            require(argc == 4, "catalog_remove_instance_lifetime expects path and event path");
            catalog_remove_with_live_instance(argv[2], argv[3]);
        } else if (scenario == "catalog_remove_library_lifetime") {
            require(argc == 4, "catalog_remove_library_lifetime expects path and event path");
            catalog_remove_with_live_library(argv[2], argv[3]);
        } else if (scenario == "library_lease") {
            require(argc == 4, "library_lease expects path and event path");
            library_lease(argv[2], argv[3]);
        } else if (scenario == "acquisition_plugin_instance") {
            require(argc == 4, "acquisition_plugin_instance expects path and event path");
            plugin_instance_lease<AcquisitionPluginInstance>(argv[2], true, argv[3]);
        } else if (scenario == "storage_plugin_instance") {
            require(argc == 4, "storage_plugin_instance expects path and event path");
            plugin_instance_lease<StoragePluginInstance>(argv[2], false, argv[3]);
        } else if (scenario == "descriptor_lifetime") {
            require(argc == 4, "descriptor_lifetime expects path and event path");
            descriptor_lifetime(argv[2], argv[3]);
        } else if (scenario == "create_failure") {
            require(argc == 3, "create_failure expects path");
            create_failure(argv[2]);
        } else if (scenario == "create_null") {
            require(argc == 3, "create_null expects path");
            create_null(argv[2]);
        } else if (scenario == "quarantine") {
            require(argc == 4, "quarantine expects path and event path");
            quarantine(argv[2], argv[3]);
        } else if (scenario == "provenance") {
            require(argc == 4, "provenance expects path and event path");
            provenance_error(argv[2], argv[3]);
        } else {
            require(false, "Unknown probe scenario");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Plugin probe failed: " << error.what() << '\n';
        return 1;
    }
}
