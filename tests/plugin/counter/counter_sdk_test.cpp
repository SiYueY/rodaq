#include "counter_interface.hpp"
#include "test_support.hpp"

#include <rodaq/plugin/c/abi.h>
#include <rodaq/plugin/export.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "service_registry_state.hpp"

namespace {

class CounterSink final {
public:
    void record(std::int64_t value) noexcept { last_value = value; }

    std::int64_t last_value = 0;
};

}  // namespace

int main() {
    using rodaq::plugin::test::CounterInterface;

    auto sink = std::make_shared<CounterSink>();
    using SinkProvider = rodaq::plugin::test::CounterSinkInterface::Provider<CounterSink>;
    auto registry_state = std::make_shared<rodaq::plugin::ServiceRegistryState>();
    RODAQ_TEST_CHECK(registry_state
                         ->add(
                             {std::string{rodaq::plugin::test::CounterSinkInterface::kInterfaceId},
                              rodaq::plugin::test::CounterSinkInterface::kMajor,
                              rodaq::plugin::test::CounterSinkInterface::kMinor, sink.get(),
                              SinkProvider::vtable(),
                              static_cast<std::uint32_t>(
                                  sizeof(rodaq::plugin::test::CounterSinkInterface::Vtable)),
                              std::shared_ptr<const void>{sink}})
                         .ok());
    RODAQ_TEST_CHECK(registry_state->freeze().ok());
    const auto service_reference = rodaq::plugin::make_service_registry_reference(registry_state);

    const auto* entry = rodaq_plugin_get_entry();
    RODAQ_TEST_CHECK(entry != nullptr);
    RODAQ_TEST_CHECK(rodaq_plugin_abi_version() == RODAQ_PLUGIN_ABI_VERSION(1u, 0u));

    uint8_t malformed_storage[2]{};
    rodaq_error_t malformed_error{};
    malformed_error.message.data = malformed_storage;
    malformed_error.message.capacity = sizeof(malformed_storage);
    malformed_error.message.size = sizeof(malformed_storage) + 1u;
    rodaq_interface_query_t malformed_query{};
    malformed_query.interface_id = rodaq_string_t{"missing", 7u};
    rodaq_interface_reference_t malformed_reference{};
    malformed_reference.context =
        reinterpret_cast<rodaq_interface_context_t*>(static_cast<std::uintptr_t>(1u));
    malformed_reference.vtable =
        reinterpret_cast<const rodaq_interface_vtable_t*>(static_cast<std::uintptr_t>(1u));
    RODAQ_TEST_CHECK(
        service_reference.vtable->query(
            service_reference.context, &malformed_query, &malformed_reference, &malformed_error) ==
        RODAQ_STATUS_INVALID_ARGUMENT);
    RODAQ_TEST_CHECK(malformed_reference.context == nullptr);
    RODAQ_TEST_CHECK(malformed_reference.vtable == nullptr);

    rodaq_plugin_instance_t rejected_instance{};
    RODAQ_TEST_CHECK(
        entry->create(&rejected_instance, &malformed_error) == RODAQ_STATUS_INVALID_ARGUMENT);
    RODAQ_TEST_CHECK(rejected_instance.context == nullptr);
    RODAQ_TEST_CHECK(rejected_instance.vtable == nullptr);

    rodaq_plugin_instance_t instance{};
    rodaq_error_t error{};
    RODAQ_TEST_CHECK(entry->create(&instance, &error) == RODAQ_STATUS_OK);
    RODAQ_TEST_CHECK(instance.context != nullptr);
    RODAQ_TEST_CHECK(instance.vtable != nullptr);

    rodaq_config_t config{};
    config.header.struct_size = sizeof(config);
    RODAQ_TEST_CHECK(
        instance.vtable->configure(
            instance.context, &service_reference, &config, &malformed_error) ==
        RODAQ_STATUS_INVALID_ARGUMENT);
    RODAQ_TEST_CHECK(
        instance.vtable->configure(instance.context, &service_reference, &config, &error) ==
        RODAQ_STATUS_OK);

    rodaq_interface_query_t query{};
    query.interface_id = rodaq_string_t{
        CounterInterface::kInterfaceId.data(),
        static_cast<std::uint32_t>(CounterInterface::kInterfaceId.size())};
    query.major = CounterInterface::kMajor;
    query.min_minor = CounterInterface::kMinMinor;
    query.max_minor = CounterInterface::kMinor;

    rodaq_interface_reference_t interface_reference{};
    RODAQ_TEST_CHECK(
        instance.vtable->query(instance.context, &query, &interface_reference, &error) ==
        RODAQ_STATUS_OK);
    RODAQ_TEST_CHECK(interface_reference.context != nullptr);
    RODAQ_TEST_CHECK(interface_reference.vtable != nullptr);

    auto* vtable = reinterpret_cast<const CounterInterface::Vtable*>(interface_reference.vtable);
    std::int64_t value = 0;
    value = 7;
    RODAQ_TEST_CHECK(
        vtable->current(interface_reference.context, &value, &malformed_error) ==
        RODAQ_STATUS_INVALID_ARGUMENT);
    RODAQ_TEST_CHECK(value == 0);
    RODAQ_TEST_CHECK(
        vtable->increment(interface_reference.context, 7, &value, &error) == RODAQ_STATUS_OK);
    RODAQ_TEST_CHECK(value == 7);
    RODAQ_TEST_CHECK(sink->last_value == 7);
    RODAQ_TEST_CHECK(
        vtable->current(interface_reference.context, &value, &error) == RODAQ_STATUS_OK);
    RODAQ_TEST_CHECK(value == 7);

    RODAQ_TEST_CHECK(
        instance.vtable->shutdown(instance.context, rodaq_deadline_t{-1}, &error) ==
        RODAQ_STATUS_INVALID_ARGUMENT);
    RODAQ_TEST_CHECK(
        instance.vtable->shutdown(
            instance.context, rodaq_deadline_t{RODAQ_DEADLINE_INFINITE}, &error) ==
        RODAQ_STATUS_OK);
    instance.vtable->destroy(instance.context);

    auto version_state = std::make_shared<rodaq::plugin::ServiceRegistryState>();
    RODAQ_TEST_CHECK(
        !version_state
             ->add(
                 {std::string{"invalid\xc0\x80", 9u}, 1u, 0u, sink.get(), SinkProvider::vtable(),
                  static_cast<std::uint32_t>(
                      sizeof(rodaq::plugin::test::CounterSinkInterface::Vtable)),
                  std::shared_ptr<const void>{sink}})
             .ok());
    RODAQ_TEST_CHECK(version_state
                         ->add(
                             {std::string{rodaq::plugin::test::CounterSinkInterface::kInterfaceId},
                              rodaq::plugin::test::CounterSinkInterface::kMajor, 2u, sink.get(),
                              SinkProvider::vtable(),
                              static_cast<std::uint32_t>(
                                  sizeof(rodaq::plugin::test::CounterSinkInterface::Vtable)),
                              std::shared_ptr<const void>{sink}})
                         .ok());
    RODAQ_TEST_CHECK(
        version_state->query(rodaq::plugin::test::CounterSinkInterface::kInterfaceId, 1u, 2u, 2u)
            .ok());
    RODAQ_TEST_CHECK(
        version_state->query(rodaq::plugin::test::CounterSinkInterface::kInterfaceId, 1u, 1u, 1u)
            .value()
            .negotiated_version.minor == 1u);
    RODAQ_TEST_CHECK(
        version_state->query(rodaq::plugin::test::CounterSinkInterface::kInterfaceId, 1u, 3u, 4u)
            .status()
            .code() == rodaq::StatusCode::Unavailable);
    RODAQ_TEST_CHECK(
        version_state->query(rodaq::plugin::test::CounterSinkInterface::kInterfaceId, 2u, 2u, 2u)
            .status()
            .code() == rodaq::StatusCode::Unavailable);
    RODAQ_TEST_CHECK(
        version_state->query("missing", 1u, 0u, 0u).status().code() == rodaq::StatusCode::NotFound);

    rodaq::plugin::authoring::OperationRegistry operations;
    RODAQ_TEST_CHECK(operations
                         .add(
                             rodaq::plugin::test::CounterInterface::kInterfaceId, 1u, 3u,
                             sink.get(), SinkProvider::vtable(),
                             static_cast<std::uint32_t>(
                                 sizeof(rodaq::plugin::test::CounterSinkInterface::Vtable)))
                         .ok());
    RODAQ_TEST_CHECK(
        operations.query(rodaq::plugin::test::CounterInterface::kInterfaceId, 1u, 2u, 2u).ok());
    RODAQ_TEST_CHECK(
        operations.query(rodaq::plugin::test::CounterInterface::kInterfaceId, 1u, 4u, 5u)
            .status()
            .code() == rodaq::StatusCode::Unavailable);
    RODAQ_TEST_CHECK(
        operations.query(rodaq::plugin::test::CounterInterface::kInterfaceId, 2u, 0u, 3u)
            .status()
            .code() == rodaq::StatusCode::Unavailable);
    RODAQ_TEST_CHECK(
        operations.query("missing", 1u, 0u, 0u).status().code() == rodaq::StatusCode::NotFound);

    return 0;
}
