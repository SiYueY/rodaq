#include <rodaq/plugin/acquisition_plugin.hpp>

#include "rodaq_test_plugin_sdk_acquisition_metadata.hpp"

#include <stdexcept>

#ifndef RODAQ_SDK_FAIL_BIND
#define RODAQ_SDK_FAIL_BIND 0
#endif
#ifndef RODAQ_SDK_FAIL_CONFIGURE
#define RODAQ_SDK_FAIL_CONFIGURE 0
#endif
#ifndef RODAQ_SDK_FAIL_START
#define RODAQ_SDK_FAIL_START 0
#endif
#ifndef RODAQ_SDK_FAIL_STOP
#define RODAQ_SDK_FAIL_STOP 0
#endif
#ifndef RODAQ_SDK_THROW_START
#define RODAQ_SDK_THROW_START 0
#endif

namespace {

class SdkAcquisitionFixture final : public rodaq::AcquisitionPlugin {
public:
    rodaq::Result<void> bind(rodaq::AcquisitionRuntime runtime) override {
        if (stage_ != Stage::Created) {
            return invalid_state();
        }
#if RODAQ_SDK_FAIL_BIND
        return failure();
#endif
        runtime_ = std::move(runtime);
        stage_ = Stage::Bound;
        return {};
    }
    rodaq::Result<void> configure(const rodaq::ConfigView& view) override {
        static_cast<void>(view);
        if (stage_ != Stage::Bound) {
            return invalid_state();
        }
#if RODAQ_SDK_FAIL_CONFIGURE
        return failure();
#endif
        stage_ = Stage::Configured;
        return {};
    }
    rodaq::Result<void> start() override {
        if (stage_ != Stage::Configured) {
            return invalid_state();
        }
#if RODAQ_SDK_THROW_START
        throw std::runtime_error("fixture start exception");
#endif
#if RODAQ_SDK_FAIL_START
        return failure();
#endif
        stage_ = Stage::Started;
        return {};
    }
    rodaq::Result<void> stop() override {
        if (stage_ != Stage::Started) {
            return invalid_state();
        }
#if RODAQ_SDK_FAIL_STOP
        return failure();
#endif
        stage_ = Stage::Stopped;
        return {};
    }
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

private:
    enum class Stage { Created, Bound, Configured, Started, Stopped };

    static rodaq::Result<void> invalid_state() {
        return nonstd::make_unexpected(
            rodaq::Error(rodaq::ErrorCode::InvalidState, "fixture", 0, "invalid lifecycle order"));
    }

    static rodaq::Result<void> failure() {
        return nonstd::make_unexpected(
            rodaq::Error(rodaq::ErrorCode::Unavailable, "fixture", 9, "fixture lifecycle failure"));
    }

    rodaq::AcquisitionRuntime runtime_;
    Stage stage_ = Stage::Created;
};

}  // namespace

RODAQ_EXPORT_ACQUISITION_PLUGIN(
    SdkAcquisitionFixture, rodaq::plugin::generated::rodaq_test_plugin_sdk_acquisition::descriptor)
