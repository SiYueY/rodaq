#include <rodaq/plugin/storage_plugin.hpp>

#include "rodaq_test_plugin_sdk_storage_metadata.hpp"

#ifndef RODAQ_SDK_STORAGE_FAIL_OPEN
#define RODAQ_SDK_STORAGE_FAIL_OPEN 0
#endif
#ifndef RODAQ_SDK_STORAGE_FAIL_FLUSH
#define RODAQ_SDK_STORAGE_FAIL_FLUSH 0
#endif
#ifndef RODAQ_SDK_STORAGE_FAIL_CLOSE
#define RODAQ_SDK_STORAGE_FAIL_CLOSE 0
#endif

namespace {

class SdkStorageFixture final : public rodaq::StoragePlugin {
public:
    rodaq::Result<void> Bind(rodaq::StorageRuntime runtime) override {
        if (stage_ != Stage::Created) {
            return invalid_state();
        }
        runtime_ = std::move(runtime);
        stage_ = Stage::Bound;
        return {};
    }
    rodaq::Result<void> Configure(const rodaq::ConfigView&) override {
        if (stage_ != Stage::Bound) {
            return invalid_state();
        }
        stage_ = Stage::Configured;
        return {};
    }
    rodaq::Result<void> Open(const rodaq_storage_open_request&) override {
        if (stage_ != Stage::Configured) {
            return invalid_state();
        }
#if RODAQ_SDK_STORAGE_FAIL_OPEN
        return failure();
#endif
        stage_ = Stage::Opened;
        return {};
    }
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
        if (stage_ != Stage::Opened) {
            return invalid_state_value();
        }
        rodaq_storage_submit_result value{};
        value.struct_size = sizeof(value);
        value.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
        value.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
        return value;
    }
    rodaq::Result<void> Flush() override {
#if RODAQ_SDK_STORAGE_FAIL_FLUSH
        return failure();
#endif
        return stage_ == Stage::Opened ? rodaq::Result<void>() : invalid_state();
    }
    rodaq::Result<void> Close(rodaq_storage_close_disposition) override {
        if (stage_ != Stage::Opened) {
            return invalid_state();
        }
#if RODAQ_SDK_STORAGE_FAIL_CLOSE
        return failure();
#endif
        stage_ = Stage::Closed;
        return {};
    }

private:
    enum class Stage { Created, Bound, Configured, Opened, Closed };

    static rodaq::Result<void> invalid_state() {
        return nonstd::make_unexpected(
            rodaq::Error(rodaq::ErrorCode::InvalidState, "fixture", 0, "invalid lifecycle order"));
    }

    static rodaq::Result<rodaq_storage_submit_result> invalid_state_value() {
        return nonstd::make_unexpected(
            rodaq::Error(rodaq::ErrorCode::InvalidState, "fixture", 0, "invalid lifecycle order"));
    }

    static rodaq::Result<void> failure() {
        return nonstd::make_unexpected(
            rodaq::Error(rodaq::ErrorCode::Unavailable, "fixture", 9, "fixture lifecycle failure"));
    }

    rodaq::StorageRuntime runtime_;
    Stage stage_ = Stage::Created;
};

}  // namespace

RODAQ_EXPORT_STORAGE_PLUGIN(
    SdkStorageFixture, rodaq::plugin::generated::rodaq_test_plugin_sdk_storage::descriptor)
