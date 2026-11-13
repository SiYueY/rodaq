#include "counter_interface.hpp"
#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>

namespace {

class CallbackSink final {
public:
    void record(std::int64_t) noexcept {
        if (during_callback) {
            const auto result = during_callback();
            status.store(result.ok() ? rodaq::StatusCode::Ok : result.status().code());
            called.store(true, std::memory_order_release);
        }
    }

    std::function<rodaq::Result<void>()> during_callback;
    std::atomic<rodaq::StatusCode> status{rodaq::StatusCode::Internal};
    std::atomic<bool> called{false};
};

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using rodaq::StatusCode;
    using rodaq::plugin::Deadline;
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
    auto library_result = manager.load("com.rodaq.test.async-service-callback", "1.0.0");
    RODAQ_TEST_CHECK(library_result.ok());
    auto library = std::move(library_result).value();

    auto sink = std::make_shared<CallbackSink>();
    sink->during_callback = [&] { return manager.shutdown(Deadline::after(10ms)); };
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.add_shared<rodaq::plugin::test::CounterSinkInterface>(sink).ok());
    RODAQ_TEST_CHECK(services.freeze().ok());
    auto instance_result = library.create_instance(services, {});
    RODAQ_TEST_CHECK(instance_result.ok());
    auto instance = std::move(instance_result).value();

    for (int attempt = 0; attempt < 100 && !sink->called.load(std::memory_order_acquire);
         ++attempt) {
        std::this_thread::sleep_for(10ms);
    }
    RODAQ_TEST_CHECK(sink->called.load(std::memory_order_acquire));
    RODAQ_TEST_CHECK(sink->status.load() == StatusCode::InvalidState);
    RODAQ_TEST_CHECK(instance.close(Deadline::after(1s)).ok());
    RODAQ_TEST_CHECK(manager.shutdown(Deadline::after(1s)).ok());
    return 0;
}
