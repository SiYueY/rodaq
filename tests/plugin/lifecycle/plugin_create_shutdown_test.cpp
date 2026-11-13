#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

int main() {
    using namespace std::chrono_literals;
    using rodaq::StatusCode;
    using rodaq::plugin::Deadline;
    using rodaq::plugin::PluginInstance;
    using rodaq::plugin::PluginLibrary;
    using rodaq::plugin::PluginManager;
    using rodaq::plugin::PluginManagerOptions;
    using rodaq::plugin::ServiceRegistry;

    PluginManagerOptions options;
    options.search_paths.push_back(std::filesystem::path{RODAQ_TEST_PLUGIN_ROOT});
    options.shutdown_timeout = 700ms;
    auto manager_result = PluginManager::create(options);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();
    RODAQ_TEST_CHECK(manager.refresh().ok());
    auto library_result = manager.load("com.rodaq.test.slow-configure", "1.0.0");
    RODAQ_TEST_CHECK(library_result.ok());
    auto library = std::move(library_result).value();
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.freeze().ok());

    std::shared_ptr<rodaq::Result<PluginInstance>> create_result;
    std::thread creator{[&] {
        create_result =
            std::make_shared<rodaq::Result<PluginInstance>>(library.create_instance(services, {}));
    }};
    std::this_thread::sleep_for(20ms);
    const auto shutdown_result = manager.shutdown(Deadline::after(2s));
    creator.join();

    RODAQ_TEST_CHECK(shutdown_result.ok());
    RODAQ_TEST_CHECK(!create_result->ok());
    RODAQ_TEST_CHECK(create_result->status().code() == StatusCode::InvalidState);
    auto diagnostics = manager.diagnostics();
    RODAQ_TEST_CHECK(diagnostics.ok());
    RODAQ_TEST_CHECK(diagnostics.value().live_instance_count == 0u);
    RODAQ_TEST_CHECK(diagnostics.value().closing_instance_count == 0u);
    return 0;
}
