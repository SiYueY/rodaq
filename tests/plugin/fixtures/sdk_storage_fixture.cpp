#include <rodaq/plugin/storage_plugin.hpp>

#include "rodaq_test_plugin_sdk_storage_metadata.hpp"

namespace {

class SdkStorageFixture final : public rodaq::StoragePlugin {
public:
    rodaq::Result<void> Bind(rodaq::StorageRuntime runtime) override {
        runtime_ = std::move(runtime);
        return {};
    }
    rodaq::Result<void> Configure(const rodaq::ConfigView&) override { return {}; }
    rodaq::Result<void> Open(const rodaq_storage_open_request&) override { return {}; }
    rodaq::Result<rodaq_storage_capabilities> GetCapabilities() override {
        rodaq_storage_capabilities value{};
        value.struct_size = sizeof(value);
        value.full_fidelity = RODAQ_TRUE;
        value.maximum_payload_bytes = 1024;
        value.maximum_inflight_records = 8;
        value.maximum_inflight_bytes = 8192;
        value.handoff_progress_timeout_ns = 1'000'000;
        value.descriptor_progress_timeout_ns = 1'000'000;
        value.flush_progress_timeout_ns = 1'000'000;
        value.close_progress_timeout_ns = 1'000'000;
        return value;
    }
    rodaq::Result<rodaq_descriptor_readiness_result> EnsureDescriptorReady(
        rodaq_stream_descriptor_handle) override {
        rodaq_descriptor_readiness_result value{};
        value.struct_size = sizeof(value);
        value.state = RODAQ_DESCRIPTOR_READY;
        return value;
    }
    rodaq::Result<rodaq_storage_submit_result> Submit(const rodaq_storage_submission&) override {
        rodaq_storage_submit_result value{};
        value.struct_size = sizeof(value);
        value.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
        value.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
        return value;
    }
    rodaq::Result<void> Flush() override { return {}; }
    rodaq::Result<void> Close(rodaq_storage_close_disposition) override { return {}; }

private:
    rodaq::StorageRuntime runtime_;
};

}  // namespace

RODAQ_EXPORT_STORAGE_PLUGIN(
    SdkStorageFixture, rodaq::plugin::generated::rodaq_test_plugin_sdk_storage::descriptor)
