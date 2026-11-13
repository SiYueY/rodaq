#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <chrono>
#include <filesystem>

int main() {
    using namespace std::chrono_literals;
    using rodaq::plugin::CloseWaitStatus;
    using rodaq::plugin::Deadline;
    using rodaq::plugin::PluginInstancePhase;
    using rodaq::plugin::PluginManager;
    using rodaq::plugin::PluginManagerOptions;
    using rodaq::plugin::ServiceRegistry;

    PluginManagerOptions options;
    options.search_paths.push_back(std::filesystem::path{RODAQ_TEST_PLUGIN_ROOT});
    options.shutdown_timeout = 500ms;
    auto manager_result = PluginManager::create(options);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();
    RODAQ_TEST_CHECK(manager.refresh().ok());
    auto library_result = manager.load("com.rodaq.test.shutdown-failure", "1.0.0");
    RODAQ_TEST_CHECK(library_result.ok());
    auto library = std::move(library_result).value();
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.freeze().ok());
    auto instance_result = library.create_instance(services, {});
    RODAQ_TEST_CHECK(instance_result.ok());
    auto instance = std::move(instance_result).value();

    auto future = instance.async_close();
    RODAQ_TEST_CHECK(future.wait(Deadline::after(1s)) == CloseWaitStatus::Completed);
    RODAQ_TEST_CHECK(!future.result().ok());
    RODAQ_TEST_CHECK(instance.phase() == PluginInstancePhase::Unresolved);
    auto diagnostics = manager.diagnostics();
    RODAQ_TEST_CHECK(diagnostics.ok());
    RODAQ_TEST_CHECK(diagnostics.value().phase == rodaq::plugin::PluginManagerPhase::Degraded);
    RODAQ_TEST_CHECK(diagnostics.value().restart_required);
    RODAQ_TEST_CHECK(!manager.shutdown(Deadline::after(500ms)).ok());
    return 0;
}
