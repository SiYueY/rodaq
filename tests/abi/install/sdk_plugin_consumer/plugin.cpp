#include <rodaq/plugin/acquisition_plugin.hpp>

#include "consumer_sdk_plugin_metadata.hpp"

namespace {

class ConsumerPlugin final : public rodaq::AcquisitionPlugin {
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

}  // namespace

RODAQ_EXPORT_ACQUISITION_PLUGIN(
    ConsumerPlugin, rodaq::plugin::generated::consumer_sdk_plugin::descriptor)
