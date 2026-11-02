#include <rodaq/plugin/acquisition_plugin.hpp>

#include "rodaq_test_plugin_sdk_acquisition_metadata.hpp"

namespace {

class SdkAcquisitionFixture final : public rodaq::AcquisitionPlugin {
public:
    rodaq::Result<void> Bind(rodaq::AcquisitionRuntime runtime) override {
        runtime_ = std::move(runtime);
        return {};
    }
    rodaq::Result<void> Configure(const rodaq::ConfigView& view) override {
        return view.Size() > 0 ? rodaq::Result<void>() : rodaq::Result<void>();
    }
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

private:
    rodaq::AcquisitionRuntime runtime_;
};

}  // namespace

RODAQ_EXPORT_ACQUISITION_PLUGIN(
    SdkAcquisitionFixture, rodaq::plugin::generated::rodaq_test_plugin_sdk_acquisition::descriptor)
