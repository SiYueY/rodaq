#include "abi/counter.hpp"

#include <rodaq/plugin/authoring.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>

// Deliberately invalid authoring example, built only for ASan qualification.
// A provided backend must outlive configure and every subsequent invocation.
class LocalCounter {
public:
    rodaq::Result<std::int64_t> increment(std::int64_t value) { return value; }

    rodaq::Result<std::int64_t> current() const { return static_cast<std::int64_t>(value_); }

private:
    volatile std::int64_t value_{42};
};

class InvalidPlugin final : public rodaq::plugin::PluginBase {
public:
    [[gnu::noinline]] rodaq::Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        LocalCounter local;
        return setup.provide<CounterInterface>(local);  // Intentionally dangling after return.
    }

    rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override { return {}; }
};

static rodaq_status_code_t no_service(
    rodaq_service_registry_context_t*, const rodaq_interface_query_t*,
    rodaq_interface_reference_t* output, rodaq_error_t*) {
    *output = {};
    return RODAQ_STATUS_NOT_FOUND;
}

int main() {
    rodaq_plugin_instance_t instance{};
    assert(
        rodaq::plugin::authoring::Instance<InvalidPlugin>::create(&instance, nullptr) ==
        RODAQ_STATUS_OK);
    int context = 0;
    const rodaq_service_registry_vtable_t table{
        {offsetof(rodaq_service_registry_vtable_t, query) + sizeof(table.query)},
        no_service,
        nullptr,
        nullptr};
    const rodaq_service_registry_reference_t services{
        reinterpret_cast<rodaq_service_registry_context_t*>(&context), &table};
    const rodaq_config_t config{{sizeof(config)}, {nullptr, 0}};
    assert(
        instance.vtable->configure(instance.context, &services, &config, nullptr) ==
        RODAQ_STATUS_OK);
    const rodaq_interface_query_t query{
        {RODAQ_COUNTER_INTERFACE_ID, sizeof(RODAQ_COUNTER_INTERFACE_ID) - 1}, 1, 0, 0};
    rodaq_interface_reference_t reference{};
    assert(
        instance.vtable->query(instance.context, &query, &reference, nullptr) == RODAQ_STATUS_OK);
    rodaq_counter_vtable_t counter{};
    assert(rodaq::plugin::abi::copy_vtable<CounterInterface>(reference, counter));
    std::int64_t value = 0;
    // Success here means the qualification failed to expose the lifetime misuse.
    return static_cast<int>(counter.current(reference.context, &value, nullptr));
}
