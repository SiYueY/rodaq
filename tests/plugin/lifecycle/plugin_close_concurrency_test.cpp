#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

#include "testing_access.hpp"

int main() {
    using namespace std::chrono_literals;
    using rodaq::plugin::CloseWaitStatus;
    using rodaq::plugin::Deadline;
    using rodaq::plugin::PluginInstancePhase;
    using rodaq::plugin::PluginManager;
    using rodaq::plugin::PluginManagerOptions;
    using rodaq::plugin::PluginManagerPhase;
    using rodaq::plugin::ServiceRegistry;

    PluginManagerOptions options;
    options.search_paths.push_back(std::filesystem::path{RODAQ_TEST_PLUGIN_ROOT});
    options.shutdown_timeout = 300ms;
    rodaq::plugin::LifecyclePolicy policy;
    policy.close_stage = 300ms;

    auto manager_result = rodaq::plugin::PluginManagerTestingAccess::create(options, policy);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();
    RODAQ_TEST_CHECK(manager.refresh().ok());

    auto hung_library_result = manager.load("com.rodaq.test.hung-shutdown", "1.0.0");
    RODAQ_TEST_CHECK(hung_library_result.ok());
    auto hung_library = std::move(hung_library_result).value();
    auto healthy_library_result = manager.load("com.rodaq.test.slow-shutdown", "1.0.0");
    RODAQ_TEST_CHECK(healthy_library_result.ok());
    auto healthy_library = std::move(healthy_library_result).value();

    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.freeze().ok());
    auto hung_instance_result = hung_library.create_instance(services, {});
    RODAQ_TEST_CHECK(hung_instance_result.ok());
    auto hung_instance = std::move(hung_instance_result).value();
    auto healthy_instance_result = healthy_library.create_instance(services, {});
    RODAQ_TEST_CHECK(healthy_instance_result.ok());
    auto healthy_instance = std::move(healthy_instance_result).value();

    auto hung_future = hung_instance.async_close();
    auto healthy_future = healthy_instance.async_close();
    RODAQ_TEST_CHECK(healthy_future.wait(Deadline::after(300ms)) == CloseWaitStatus::Completed);
    RODAQ_TEST_CHECK(healthy_future.result().ok());
    RODAQ_TEST_CHECK(healthy_instance.phase() == PluginInstancePhase::Closed);
    RODAQ_TEST_CHECK(!hung_future.ready());

    std::this_thread::sleep_for(650ms);
    auto diagnostics = manager.diagnostics();
    RODAQ_TEST_CHECK(diagnostics.ok());
    RODAQ_TEST_CHECK(diagnostics.value().phase == PluginManagerPhase::Degraded);
    RODAQ_TEST_CHECK(diagnostics.value().restart_required);
    RODAQ_TEST_CHECK(!manager.shutdown(Deadline::after(100ms)).ok());
    return 0;
}
