#include <rodaq/plugin/acquisition_plugin.hpp>
#include <rodaq/plugin/config_view.hpp>
#include <rodaq/plugin/plugin.hpp>
#include <rodaq/plugin/runtime_services.hpp>
#include <rodaq/plugin/service_registry.hpp>
#include <rodaq/plugin/storage_plugin.hpp>

namespace {

class AcquisitionStub final : public rodaq::AcquisitionPlugin {
public:
    rodaq::Result<void> bind(rodaq::AcquisitionRuntime) override { return {}; }
    rodaq::Result<void> configure(const rodaq::ConfigView&) override { return {}; }
    rodaq::Result<void> start() override { return {}; }
    rodaq::Result<void> stop() override { return {}; }
    rodaq::Result<void> prepare_binding_switch(
        rodaq_binding_transition, rodaq_stream_handle, rodaq_stream_descriptor_handle,
        rodaq_stream_descriptor_handle) override {
        return {};
    }
    rodaq::Result<void> commit_binding_switch(rodaq_binding_transition) override { return {}; }
    rodaq::Result<void> abort_binding_switch(
        rodaq_binding_transition, rodaq_binding_abort_disposition) override {
        return {};
    }
    rodaq::Result<void> quiesce_stream(rodaq_stream_handle) override { return {}; }
    rodaq::Result<void> quiesce_plugin() override { return {}; }
};

class StorageStub final : public rodaq::StoragePlugin {
public:
    rodaq::Result<void> bind(rodaq::StorageRuntime) override { return {}; }
    rodaq::Result<void> configure(const rodaq::ConfigView&) override { return {}; }
    rodaq::Result<void> open(const rodaq_storage_open_request&) override { return {}; }
    rodaq::Result<rodaq_storage_capabilities> get_capabilities() override {
        return rodaq_storage_capabilities{};
    }
    rodaq::Result<rodaq_descriptor_readiness_result> ensure_descriptor_ready(
        rodaq_stream_descriptor_handle) override {
        return rodaq_descriptor_readiness_result{};
    }
    rodaq::Result<rodaq_storage_submit_result> submit(const rodaq_storage_submission&) override {
        return rodaq_storage_submit_result{};
    }
    rodaq::Result<void> flush() override { return {}; }
    rodaq::Result<void> close(rodaq_storage_close_disposition) override { return {}; }
};

}  // namespace

int main() {
    rodaq::ConfigView config;
    rodaq::ServiceRegistryView registry;
    return config.find("key").has_value() || registry.query("service").has_value() ? 1 : 0;
}
