#include "counter_interface.hpp"
#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "testing_access.hpp"

namespace {

class CounterSink final {
public:
    void record(std::int64_t) noexcept {}
};

std::filesystem::path make_hung_refresh_root() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("rodaq-plugin-refresh-" + std::to_string(::getpid()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::filesystem::copy(
        std::filesystem::path{RODAQ_TEST_COUNTER_PACKAGE}, root / "counter",
        std::filesystem::copy_options::recursive);
    return root;
}

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using rodaq::StatusCode;
    using rodaq::plugin::Deadline;
    using rodaq::plugin::PluginManager;
    using rodaq::plugin::PluginManagerOptions;
    using rodaq::plugin::ServiceRegistry;

    const auto root = make_hung_refresh_root();
    auto sink = std::make_shared<CounterSink>();
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.add_shared<rodaq::plugin::test::CounterSinkInterface>(sink).ok());
    RODAQ_TEST_CHECK(services.freeze().ok());

    PluginManagerOptions options;
    options.search_paths.push_back(root);
    options.shutdown_timeout = 2s;
    rodaq::plugin::LifecyclePolicy policy;
    policy.scheduling = 20ms;
    policy.catalog_stage = 60ms;
    auto manager_result = rodaq::plugin::PluginManagerTestingAccess::create(options, policy);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();
    RODAQ_TEST_CHECK(manager.refresh().ok());

    auto library_result = manager.load("com.rodaq.test.counter", "1.0.0");
    RODAQ_TEST_CHECK(library_result.ok());
    auto library = std::move(library_result).value();
    auto instance_result = library.create_instance(services, {});
    RODAQ_TEST_CHECK(instance_result.ok());
    auto instance = std::move(instance_result).value();

    const auto hung_package = root / "hung";
    std::filesystem::create_directories(hung_package);
    const auto fifo = hung_package / "manifest.json";
    RODAQ_TEST_CHECK(::mkfifo(fifo.c_str(), 0600) == 0);

    rodaq::Result<void> first_refresh = rodaq::Status{StatusCode::Internal, "not run"};
    rodaq::Result<void> second_refresh = rodaq::Status{StatusCode::Internal, "not run"};
    std::thread first{[&] { first_refresh = manager.refresh(); }};
    std::thread second{[&] { second_refresh = manager.refresh(); }};

    std::this_thread::sleep_for(20ms);
    const auto shutdown = manager.shutdown(Deadline::after(200ms));
    RODAQ_TEST_CHECK(!shutdown.ok());

    first.join();
    second.join();
    RODAQ_TEST_CHECK(!first_refresh.ok());
    RODAQ_TEST_CHECK(!second_refresh.ok());
    auto close_future = instance.async_close();
    RODAQ_TEST_CHECK(
        close_future.wait(Deadline::after(1s)) == rodaq::plugin::CloseWaitStatus::Completed);
    RODAQ_TEST_CHECK(instance.phase() == rodaq::plugin::PluginInstancePhase::Closed);
    return 0;
}
