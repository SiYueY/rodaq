#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>

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
    options.max_instances = 1u;
    options.shutdown_timeout = 500ms;
    auto manager_result = PluginManager::create(options);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();
    RODAQ_TEST_CHECK(manager.refresh().ok());
    auto library_result = manager.load("com.rodaq.test.configure-failure", "1.0.0");
    RODAQ_TEST_CHECK(library_result.ok());
    auto library = std::move(library_result).value();
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.freeze().ok());

    auto first = library.create_instance(services, {});
    RODAQ_TEST_CHECK(!first.ok());
    RODAQ_TEST_CHECK(first.status().code() == StatusCode::InvalidArgument);

    std::this_thread::sleep_for(100ms);
    auto second = library.create_instance(services, {});
    RODAQ_TEST_CHECK(!second.ok());
    RODAQ_TEST_CHECK(second.status().code() == StatusCode::InvalidArgument);
    RODAQ_TEST_CHECK(manager.shutdown(Deadline::after(2s)).ok());
    return 0;
}
