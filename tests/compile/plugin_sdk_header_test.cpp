#include <rodaq/plugin/acquisition_plugin.hpp>
#include <rodaq/plugin/config_view.hpp>
#include <rodaq/plugin/plugin.hpp>
#include <rodaq/plugin/runtime_services.hpp>
#include <rodaq/plugin/service_registry.hpp>
#include <rodaq/plugin/storage_plugin.hpp>

namespace {

class AcquisitionStub final : public rodaq::AcquisitionPlugin {
public:
    rodaq::Result<void> Bind(rodaq::AcquisitionRuntime) override { return {}; }
    rodaq::Result<void> Configure(const rodaq::ConfigView&) override { return {}; }
    rodaq::Result<void> Start() override { return {}; }
    rodaq::Result<void> Stop() override { return {}; }
    rodaq::Result<void> PrepareBindingSwitch(
        rodaq_binding_transition, rodaq_stream_handle, rodaq_stream_descriptor_handle,
        rodaq_stream_descriptor_handle) override {
        return {};
    }
    rodaq::Result<void> CommitBindingSwitch(rodaq_binding_transition) override { return {}; }
    rodaq::Result<void> AbortBindingSwitch(
        rodaq_binding_transition, rodaq_binding_abort_disposition) override {
        return {};
    }
    rodaq::Result<void> QuiesceStream(rodaq_stream_handle) override { return {}; }
    rodaq::Result<void> QuiescePlugin() override { return {}; }
};

class StorageStub final : public rodaq::StoragePlugin {
public:
    rodaq::Result<void> Bind(rodaq::StorageRuntime) override { return {}; }
    rodaq::Result<void> Configure(const rodaq::ConfigView&) override { return {}; }
    rodaq::Result<void> Open(const rodaq_storage_open_request&) override { return {}; }
    rodaq::Result<rodaq_storage_capabilities> GetCapabilities() override {
        return rodaq_storage_capabilities{};
    }
    rodaq::Result<rodaq_descriptor_readiness_result> EnsureDescriptorReady(
        rodaq_stream_descriptor_handle) override {
        return rodaq_descriptor_readiness_result{};
    }
    rodaq::Result<rodaq_storage_submit_result> Submit(const rodaq_storage_submission&) override {
        return rodaq_storage_submit_result{};
    }
    rodaq::Result<void> Flush() override { return {}; }
    rodaq::Result<void> Close(rodaq_storage_close_disposition) override { return {}; }
};

}  // namespace

int main() {
    rodaq::ConfigView config;
    rodaq::ServiceRegistryView registry;
    return config.Find("key").has_value() || registry.Query("service").has_value() ? 1 : 0;
}
