#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include "testing_access.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

int main() {
    using namespace std::chrono_literals;
    using rodaq::StatusCode;
    using rodaq::plugin::Deadline;
    using rodaq::plugin::PluginManager;
    using rodaq::plugin::PluginManagerOptions;
    using rodaq::plugin::ServiceRegistry;

    PluginManagerOptions options;
    options.search_paths.push_back(std::filesystem::path{RODAQ_TEST_PLUGIN_ROOT});
    options.shutdown_timeout = 2s;
    rodaq::plugin::LifecyclePolicy policy;
    policy.scheduling = 20ms;
    policy.initialization_stage = 80ms;
    auto manager_result = rodaq::plugin::PluginManagerTestingAccess::create(options, policy);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();
    RODAQ_TEST_CHECK(manager.refresh().ok());

    auto library_result = manager.load("com.rodaq.test.slow-configure", "1.0.0");
    RODAQ_TEST_CHECK(library_result.ok());
    auto library = std::move(library_result).value();
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.freeze().ok());
    auto instance = library.create_instance(services, {});
    RODAQ_TEST_CHECK(!instance.ok());
    RODAQ_TEST_CHECK(instance.status().code() == StatusCode::DeadlineExceeded);

    auto diagnostics = manager.diagnostics();
    RODAQ_TEST_CHECK(diagnostics.ok());
    RODAQ_TEST_CHECK(diagnostics.value().phase == rodaq::plugin::PluginManagerPhase::Degraded);
    RODAQ_TEST_CHECK(diagnostics.value().restart_required);

    std::this_thread::sleep_for(600ms);
    RODAQ_TEST_CHECK(!manager.shutdown(Deadline::after(200ms)).ok());
    return 0;
}
