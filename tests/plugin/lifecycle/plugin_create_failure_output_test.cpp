#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <chrono>
#include <filesystem>

int main() {
    using namespace std::chrono_literals;
    using rodaq::StatusCode;
    using rodaq::plugin::Deadline;
    using rodaq::plugin::PluginManager;
    using rodaq::plugin::PluginManagerOptions;
    using rodaq::plugin::PluginManagerPhase;
    using rodaq::plugin::ServiceRegistry;

    PluginManagerOptions options;
    options.search_paths.push_back(std::filesystem::path{RODAQ_TEST_PLUGIN_ROOT});
    options.shutdown_timeout = 200ms;
    auto manager_result = PluginManager::create(options);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();
    RODAQ_TEST_CHECK(manager.refresh().ok());
    auto library_result = manager.load("com.rodaq.test.partial-create-failure", "1.0.0");
    RODAQ_TEST_CHECK(library_result.ok());
    auto library = std::move(library_result).value();
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.freeze().ok());

    auto create_result = library.create_instance(services, {});
    RODAQ_TEST_CHECK(!create_result.ok());
    RODAQ_TEST_CHECK(create_result.status().code() == StatusCode::Internal);
    auto diagnostics = manager.diagnostics();
    RODAQ_TEST_CHECK(diagnostics.ok());
    RODAQ_TEST_CHECK(diagnostics.value().phase == PluginManagerPhase::Degraded);
    RODAQ_TEST_CHECK(diagnostics.value().restart_required);
    RODAQ_TEST_CHECK(diagnostics.value().faulted_plugin_count == 1u);
    RODAQ_TEST_CHECK(!manager.shutdown(Deadline::after(500ms)).ok());
    return 0;
}
