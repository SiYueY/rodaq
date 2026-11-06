#include <rodaq/plugin/acquisition_plugin.hpp>

#include "consumer_sdk_plugin_metadata.hpp"

namespace {

class ConsumerPlugin final : public rodaq::plugin::AcquisitionPlugin {
public:
    rodaq::Result<void> bind(rodaq::plugin::AcquisitionContext) override { return {}; }
    rodaq::Result<void> configure(const rodaq::plugin::ConfigView&) override { return {}; }
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

}  // namespace

RODAQ_EXPORT_ACQUISITION_PLUGIN(
    ConsumerPlugin, rodaq::plugin::generated::consumer_sdk_plugin::descriptor)
