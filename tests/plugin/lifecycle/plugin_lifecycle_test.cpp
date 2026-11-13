#include "counter_interface.hpp"
#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <limits>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <dlfcn.h>

namespace {

std::size_t thread_count() {
    std::size_t count = 0u;
    for (const auto& entry : std::filesystem::directory_iterator{"/proc/self/task"}) {
        (void)entry;
        ++count;
    }
    return count;
}

class CounterSink final {
public:
    void record(std::int64_t value) noexcept {
        last_value = value;
        if (during_record) {
            const auto result = during_record();
            self_wait_code.store(result.ok() ? rodaq::StatusCode::Ok : result.status().code());
        }
    }

    std::atomic<std::int64_t> last_value{0};
    std::atomic<rodaq::StatusCode> self_wait_code{rodaq::StatusCode::Ok};
    std::function<rodaq::Result<void>()> during_record;
};

std::atomic<bool> validation_entered{false};
std::atomic<bool> validation_released{false};

struct GuardedCounterInterface {
    using Vtable = rodaq::plugin::test::CounterInterface::Vtable;
    using Handle = rodaq::plugin::OperationHandle<GuardedCounterInterface>;

    static constexpr std::string_view kInterfaceId =
        rodaq::plugin::test::CounterInterface::kInterfaceId;
    static constexpr std::uint16_t kMajor = rodaq::plugin::test::CounterInterface::kMajor;
    static constexpr std::uint16_t kMinor = rodaq::plugin::test::CounterInterface::kMinor;
    static constexpr std::uint16_t kMinMinor = rodaq::plugin::test::CounterInterface::kMinMinor;

    static constexpr std::size_t required_prefix_size() noexcept {
        return rodaq::plugin::test::CounterInterface::required_prefix_size();
    }

    static bool required_slots_valid(const Vtable* vtable) noexcept {
        validation_entered.store(true, std::memory_order_release);
        while (!validation_released.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return rodaq::plugin::test::CounterInterface::required_slots_valid(vtable);
    }
};

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using rodaq::StatusCode;
    using rodaq::plugin::Deadline;
    using rodaq::plugin::PluginManager;
    using rodaq::plugin::PluginManagerOptions;
    using rodaq::plugin::ServiceRegistry;
    using rodaq::plugin::test::CounterInterface;

    const auto baseline_threads = thread_count();
    PluginManagerOptions invalid_options;
    invalid_options.max_config_size =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
    auto invalid_manager = PluginManager::create(invalid_options);
    RODAQ_TEST_CHECK(!invalid_manager.ok());
    RODAQ_TEST_CHECK(invalid_manager.status().code() == StatusCode::InvalidArgument);

    auto sink = std::make_shared<CounterSink>();
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.add_shared<rodaq::plugin::test::CounterSinkInterface>(sink).ok());
    RODAQ_TEST_CHECK(services.freeze().ok());

    PluginManagerOptions options;
    options.search_paths.push_back(std::filesystem::path{RODAQ_TEST_PLUGIN_ROOT});
    options.max_loaded_plugins = 1u;
    options.max_instances = 2u;
    options.max_config_size = 4u;
    options.shutdown_timeout = 500ms;

    auto manager_result = PluginManager::create(options);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();

    auto duplicate_manager = PluginManager::create(options);
    RODAQ_TEST_CHECK(!duplicate_manager.ok());
    RODAQ_TEST_CHECK(duplicate_manager.status().code() == StatusCode::Conflict);

    RODAQ_TEST_CHECK(manager.refresh().ok());
    auto plugins = manager.plugins();
    RODAQ_TEST_CHECK(plugins.ok());
    bool found_counter = false;
    for (const auto& plugin : plugins.value()) {
        if (plugin.plugin_id == "com.rodaq.test.counter") {
            found_counter = true;
        }
    }
    RODAQ_TEST_CHECK(found_counter);

    auto first_load = std::make_shared<rodaq::Result<rodaq::plugin::PluginLibrary>>(
        rodaq::Status{rodaq::StatusCode::Internal, "load not run"});
    auto second_load = std::make_shared<rodaq::Result<rodaq::plugin::PluginLibrary>>(
        rodaq::Status{rodaq::StatusCode::Internal, "load not run"});
    std::thread first_loader{
        [&] { *first_load = manager.load("com.rodaq.test.counter", "1.0.0"); }};
    std::thread second_loader{
        [&] { *second_load = manager.load("com.rodaq.test.counter", "1.0.0"); }};
    first_loader.join();
    second_loader.join();
    RODAQ_TEST_CHECK(first_load->ok());
    RODAQ_TEST_CHECK(second_load->ok());
    auto post_load_diagnostics = manager.diagnostics();
    RODAQ_TEST_CHECK(post_load_diagnostics.ok());
    RODAQ_TEST_CHECK(post_load_diagnostics.value().loaded_plugin_count == 1u);
    auto library = std::move(*first_load).value();
    RODAQ_TEST_CHECK(library.info().plugin_id == "com.rodaq.test.counter");
    RODAQ_TEST_CHECK(library.artifact().sha256.size() == 64u);
    auto over_loaded = manager.load("com.rodaq.test.slow-shutdown", "1.0.0");
    RODAQ_TEST_CHECK(!over_loaded.ok());
    RODAQ_TEST_CHECK(over_loaded.status().code() == StatusCode::ResourceExhausted);
    auto oversized_config = library.create_instance(services, "12345");
    RODAQ_TEST_CHECK(!oversized_config.ok());
    RODAQ_TEST_CHECK(oversized_config.status().code() == StatusCode::ResourceExhausted);
    auto invalid_utf8_config = library.create_instance(services, std::string{"\xc0\x80", 2u});
    RODAQ_TEST_CHECK(!invalid_utf8_config.ok());
    RODAQ_TEST_CHECK(invalid_utf8_config.status().code() == StatusCode::InvalidArgument);

    auto instance_result = library.create_instance(services, {});
    RODAQ_TEST_CHECK(instance_result.ok());
    auto instance = std::move(instance_result).value();
    RODAQ_TEST_CHECK(instance.phase() == rodaq::plugin::PluginInstancePhase::Open);

    auto operation_result = instance.query<CounterInterface>();
    RODAQ_TEST_CHECK(operation_result.ok());
    auto operation = std::move(operation_result).value();
    auto increment_result = rodaq::plugin::test::increment(operation, 7);
    RODAQ_TEST_CHECK(increment_result.ok());
    RODAQ_TEST_CHECK(increment_result.value() == 7);
    RODAQ_TEST_CHECK(sink->last_value.load() == 7);

    auto future = instance.async_close();
    RODAQ_TEST_CHECK(future.wait(Deadline::after(2s)) == rodaq::plugin::CloseWaitStatus::Completed);
    RODAQ_TEST_CHECK(future.result().ok());
    RODAQ_TEST_CHECK(instance.phase() == rodaq::plugin::PluginInstancePhase::Closed);
    RODAQ_TEST_CHECK(instance.close(Deadline::after(1s)).ok());

    auto second_instance_result = library.create_instance(services, {});
    RODAQ_TEST_CHECK(second_instance_result.ok());
    auto second_instance = std::move(second_instance_result).value();
    RODAQ_TEST_CHECK(second_instance.phase() == rodaq::plugin::PluginInstancePhase::Open);
    auto second_operation_result = second_instance.query<CounterInterface>();
    RODAQ_TEST_CHECK(second_operation_result.ok());
    auto second_operation = std::move(second_operation_result).value();
    sink->during_record = [&] { return manager.shutdown(Deadline::after(10ms)); };
    RODAQ_TEST_CHECK(rodaq::plugin::test::increment(second_operation, 1).ok());
    RODAQ_TEST_CHECK(sink->self_wait_code.load() == StatusCode::InvalidState);
    sink->during_record = [&] { return second_instance.close(Deadline::after(10ms)); };
    RODAQ_TEST_CHECK(rodaq::plugin::test::increment(second_operation, 1).ok());
    RODAQ_TEST_CHECK(sink->self_wait_code.load() == StatusCode::InvalidState);
    sink->during_record = {};

    std::shared_ptr<rodaq::Result<GuardedCounterInterface::Handle>> guarded_query;
    std::thread guarded_query_thread{[&] {
        guarded_query = std::make_shared<rodaq::Result<GuardedCounterInterface::Handle>>(
            second_instance.query<GuardedCounterInterface>());
    }};
    while (!validation_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    auto guarded_close = second_instance.async_close();
    std::this_thread::sleep_for(20ms);
    RODAQ_TEST_CHECK(!guarded_close.ready());
    validation_released.store(true, std::memory_order_release);
    guarded_query_thread.join();
    RODAQ_TEST_CHECK(guarded_query->ok());
    RODAQ_TEST_CHECK(
        guarded_close.wait(Deadline::after(1s)) == rodaq::plugin::CloseWaitStatus::Completed);
    RODAQ_TEST_CHECK(guarded_close.result().ok());

    auto replacement_instance_result = library.create_instance(services, {});
    RODAQ_TEST_CHECK(replacement_instance_result.ok());
    auto replacement_instance = std::move(replacement_instance_result).value();

    auto third_instance_result = library.create_instance(services, {});
    RODAQ_TEST_CHECK(third_instance_result.ok());
    auto third_instance = std::move(third_instance_result).value();
    RODAQ_TEST_CHECK(third_instance.phase() == rodaq::plugin::PluginInstancePhase::Open);
    auto third_operation_result = third_instance.query<CounterInterface>();
    RODAQ_TEST_CHECK(third_operation_result.ok());
    auto third_operation = std::move(third_operation_result).value();

    auto over_capacity = library.create_instance(services, {});
    RODAQ_TEST_CHECK(!over_capacity.ok());
    RODAQ_TEST_CHECK(over_capacity.status().code() == StatusCode::ResourceExhausted);

    std::atomic<bool> stop_calls{false};
    std::atomic<std::size_t> accepted_calls{0u};
    std::atomic<std::size_t> rejected_calls{0u};
    std::thread caller{[&] {
        while (!stop_calls.load(std::memory_order_acquire)) {
            auto result = rodaq::plugin::test::increment(third_operation, 1);
            if (result.ok()) {
                accepted_calls.fetch_add(1u, std::memory_order_relaxed);
            } else {
                rejected_calls.fetch_add(1u, std::memory_order_relaxed);
            }
        }
    }};

    std::this_thread::sleep_for(10ms);
    RODAQ_TEST_CHECK(manager.shutdown(Deadline::after(3s)).ok());
    RODAQ_TEST_CHECK(manager.shutdown(Deadline::after(3s)).ok());
    stop_calls.store(true, std::memory_order_release);
    caller.join();
    RODAQ_TEST_CHECK(accepted_calls.load() > 0u);
    RODAQ_TEST_CHECK(rejected_calls.load() > 0u);
    RODAQ_TEST_CHECK(second_instance.phase() == rodaq::plugin::PluginInstancePhase::Closed);
    RODAQ_TEST_CHECK(third_instance.phase() == rodaq::plugin::PluginInstancePhase::Closed);

    void* plugin_handle = ::dlopen(RODAQ_TEST_COUNTER_ARTIFACT, RTLD_NOW | RTLD_LOCAL);
    RODAQ_TEST_CHECK(plugin_handle != nullptr);
    using DestroyCountFn = int (*)();
    const auto destroy_count = reinterpret_cast<DestroyCountFn>(
        ::dlsym(plugin_handle, "rodaq_test_counter_destroy_count"));
    RODAQ_TEST_CHECK(destroy_count != nullptr);
    RODAQ_TEST_CHECK(destroy_count() == 4);
    std::this_thread::sleep_for(20ms);
    RODAQ_TEST_CHECK(thread_count() <= baseline_threads + 1u);
    return 0;
}
