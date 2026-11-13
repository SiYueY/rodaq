#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/interface.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace {

struct UnknownVtable {
    rodaq_struct_header_t header;
    rodaq_status_code_t (*call)(rodaq_interface_context_t*, rodaq_error_t*);
};

struct UnknownInterface {
    using Vtable = UnknownVtable;

    static constexpr std::string_view kInterfaceId = "com.rodaq.test.unknown-operation";
    static constexpr std::uint16_t kMajor = 1;
    static constexpr std::uint16_t kMinor = 0;
    static constexpr std::uint16_t kMinMinor = 0;

    static constexpr std::size_t required_prefix_size() noexcept { return sizeof(Vtable); }
    static bool required_slots_valid(const Vtable* vtable) noexcept {
        return vtable != nullptr && vtable->call != nullptr;
    }
};

}  // namespace

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
    options.shutdown_timeout = 1s;
    auto manager_result = PluginManager::create(options);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();
    RODAQ_TEST_CHECK(manager.refresh().ok());
    auto library_result = manager.load("com.rodaq.test.unknown-operation-status", "1.0.0");
    RODAQ_TEST_CHECK(library_result.ok());
    auto library = std::move(library_result).value();
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.freeze().ok());
    auto instance_result = library.create_instance(services, {});
    RODAQ_TEST_CHECK(instance_result.ok());
    auto instance = std::move(instance_result).value();
    auto operation_result = instance.query<UnknownInterface>();
    RODAQ_TEST_CHECK(operation_result.ok());
    auto operation = std::move(operation_result).value();
    auto call_result =
        operation.invoke([](rodaq_interface_context_t* context, const UnknownVtable* vtable,
                            rodaq_error_t* error) { return vtable->call(context, error); });
    RODAQ_TEST_CHECK(!call_result.ok());
    RODAQ_TEST_CHECK(call_result.status().code() == StatusCode::Internal);
    auto diagnostics = manager.diagnostics();
    RODAQ_TEST_CHECK(diagnostics.ok());
    RODAQ_TEST_CHECK(diagnostics.value().phase == PluginManagerPhase::Degraded);
    RODAQ_TEST_CHECK(!diagnostics.value().recent_errors.empty());
    RODAQ_TEST_CHECK(!manager.shutdown(Deadline::after(500ms)).ok());
    return 0;
}
