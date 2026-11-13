#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <utility>

int main() {
    using namespace std::chrono_literals;
    using rodaq::StatusCode;
    using rodaq::plugin::CloseWaitStatus;
    using rodaq::plugin::Deadline;
    using rodaq::plugin::PluginInstancePhase;
    using rodaq::plugin::PluginManager;
    using rodaq::plugin::PluginManagerOptions;
    using rodaq::plugin::ServiceRegistry;

    PluginManagerOptions options;
    options.search_paths.push_back(std::filesystem::path{RODAQ_TEST_PLUGIN_ROOT});
    options.shutdown_timeout = 1s;
    auto manager_result = PluginManager::create(options);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();
    RODAQ_TEST_CHECK(manager.refresh().ok());
    auto library_result = manager.load("com.rodaq.test.slow-shutdown", "1.0.0");
    RODAQ_TEST_CHECK(library_result.ok());
    auto library = std::move(library_result).value();
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.freeze().ok());
    auto instance_result = library.create_instance(services, {});
    RODAQ_TEST_CHECK(instance_result.ok());
    auto instance = std::move(instance_result).value();

    auto future = instance.async_close();
    auto observer = future;
    RODAQ_TEST_CHECK(observer);
    RODAQ_TEST_CHECK(!future.ready());
    RODAQ_TEST_CHECK(future.result().status().code() == StatusCode::InvalidState);
    RODAQ_TEST_CHECK(future.wait(Deadline::after(10ms)) == CloseWaitStatus::TimedOut);
    auto moved_observer = std::move(observer);
    RODAQ_TEST_CHECK(!observer);
    RODAQ_TEST_CHECK(observer.wait(Deadline::after(1ms)) == CloseWaitStatus::TimedOut);
    RODAQ_TEST_CHECK(observer.result().status().code() == StatusCode::InvalidState);
    RODAQ_TEST_CHECK(future.wait(Deadline::after(1s)) == CloseWaitStatus::Completed);
    RODAQ_TEST_CHECK(moved_observer.wait(Deadline::after(1s)) == CloseWaitStatus::Completed);
    RODAQ_TEST_CHECK(future.result().ok());
    RODAQ_TEST_CHECK(moved_observer.result().ok());
    RODAQ_TEST_CHECK(instance.phase() == PluginInstancePhase::Closed);

    auto destructor_instance_result = library.create_instance(services, {});
    RODAQ_TEST_CHECK(destructor_instance_result.ok());
    auto destructor_instance = std::make_unique<rodaq::plugin::PluginInstance>(
        std::move(destructor_instance_result).value());
    const auto before = std::chrono::steady_clock::now();
    destructor_instance.reset();
    const auto elapsed = std::chrono::steady_clock::now() - before;
    RODAQ_TEST_CHECK(elapsed < 50ms);

    RODAQ_TEST_CHECK(manager.shutdown(Deadline::after(1s)).ok());
    return 0;
}
