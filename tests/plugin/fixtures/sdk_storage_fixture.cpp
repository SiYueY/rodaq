#include <rodaq/plugin/storage_plugin.hpp>

#include "rodaq_test_plugin_sdk_storage_metadata.hpp"

#include <exception>
#include <new>
#include <stdexcept>

#ifndef RODAQ_SDK_STORAGE_FAIL_OPEN
#define RODAQ_SDK_STORAGE_FAIL_OPEN 0
#endif
#ifndef RODAQ_SDK_STORAGE_FAIL_FLUSH
#define RODAQ_SDK_STORAGE_FAIL_FLUSH 0
#endif
#ifndef RODAQ_SDK_STORAGE_FAIL_CLOSE
#define RODAQ_SDK_STORAGE_FAIL_CLOSE 0
#endif
#ifndef RODAQ_SDK_STORAGE_THROW_METHOD
#define RODAQ_SDK_STORAGE_THROW_METHOD 0
#endif
#ifndef RODAQ_SDK_STORAGE_THROW_KIND
#define RODAQ_SDK_STORAGE_THROW_KIND 0
#endif
#ifndef RODAQ_SDK_STORAGE_ALLOW_DIRECT_OPEN
#define RODAQ_SDK_STORAGE_ALLOW_DIRECT_OPEN 0
#endif

namespace {

enum class ThrowMethod : int {
    None = 0,
    Submit = 1,
    Capabilities = 2,
    DescriptorReady = 3,
};

[[noreturn]] void throw_storage_exception() {
#if RODAQ_SDK_STORAGE_THROW_KIND == 1
    throw std::bad_alloc();
#elif RODAQ_SDK_STORAGE_THROW_KIND == 2
    throw std::runtime_error("fixture storage runtime exception");
#elif RODAQ_SDK_STORAGE_THROW_KIND == 3
    throw 42;
#else
    std::terminate();
#endif
}

void maybe_throw(ThrowMethod method) {
    if constexpr (RODAQ_SDK_STORAGE_THROW_METHOD != 0) {
        if (static_cast<int>(method) == RODAQ_SDK_STORAGE_THROW_METHOD) {
            throw_storage_exception();
        }
    }
}

class SdkStorageFixture final : public rodaq::plugin::StoragePlugin {
public:
    rodaq::Result<void> bind(rodaq::plugin::StorageContext runtime) override {
        if (stage_ != Stage::Created) {
            return invalid_state();
        }
        runtime_ = std::move(runtime);
        stage_ = Stage::Bound;
        return {};
    }
    rodaq::Result<void> configure(const rodaq::plugin::ConfigView&) override {
        if (stage_ != Stage::Bound) {
            return invalid_state();
        }
        stage_ = Stage::Configured;
        return {};
    }
    rodaq::Result<void> open(const rodaq_storage_open_request&) override {
#if RODAQ_SDK_STORAGE_ALLOW_DIRECT_OPEN
        if (stage_ == Stage::Created) {
            stage_ = Stage::Opened;
            return {};
        }
#endif
        if (stage_ != Stage::Configured) {
            return invalid_state();
        }
#if RODAQ_SDK_STORAGE_FAIL_OPEN
        return failure();
#endif
        stage_ = Stage::Opened;
        return {};
    }
    rodaq::Result<rodaq_storage_capabilities> get_capabilities() override {
        maybe_throw(ThrowMethod::Capabilities);
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
    rodaq::Result<rodaq_descriptor_readiness_result> ensure_descriptor_ready(
        rodaq_stream_descriptor_handle) override {
        maybe_throw(ThrowMethod::DescriptorReady);
        rodaq_descriptor_readiness_result value{};
        value.struct_size = sizeof(value);
        value.state = RODAQ_DESCRIPTOR_READY;
        return value;
    }
    rodaq::Result<rodaq_storage_submit_result> submit(const rodaq_storage_submission&) override {
        maybe_throw(ThrowMethod::Submit);
        if (stage_ != Stage::Opened) {
            return invalid_state_value();
        }
        rodaq_storage_submit_result value{};
        value.struct_size = sizeof(value);
        value.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
        value.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
        return value;
    }
    rodaq::Result<void> flush() override {
#if RODAQ_SDK_STORAGE_FAIL_FLUSH
        return failure();
#endif
        if (stage_ != Stage::Opened) {
            return invalid_state();
        }
        return {};
    }
    rodaq::Result<void> close(rodaq_storage_close_disposition) override {
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

    static rodaq::Status invalid_state() {
        return rodaq::Status(
            rodaq::StatusCode::InvalidState, "fixture", 0, "invalid lifecycle order");
    }

    static rodaq::Result<rodaq_storage_submit_result> invalid_state_value() {
        return rodaq::Status(
            rodaq::StatusCode::InvalidState, "fixture", 0, "invalid lifecycle order");
    }

    static rodaq::Status failure() {
        return rodaq::Status(
            rodaq::StatusCode::Unavailable, "fixture", 9, "fixture lifecycle failure");
    }

    rodaq::plugin::StorageContext runtime_;
    Stage stage_ = Stage::Created;
};

}  // namespace

RODAQ_EXPORT_STORAGE_PLUGIN(
    SdkStorageFixture, rodaq::plugin::generated::rodaq_test_plugin_sdk_storage::descriptor)
