#include <rodaq/plugin/storage_plugin.hpp>

#include "rodaq_fake_storage_plugin_metadata.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::string config_string(
    const rodaq::ConfigView& view, std::string_view key, std::string default_value) {
    auto child = view.find(key);
    if (!child.has_value()) {
        return default_value;
    }
    auto value = child.value().as_string();
    return value.has_value() ? std::string(value.value()) : std::move(default_value);
}

std::int64_t config_int(
    const rodaq::ConfigView& view, std::string_view key, std::int64_t default_value) {
    auto child = view.find(key);
    if (!child.has_value()) {
        return default_value;
    }
    auto value = child.value().as_int64();
    return value.has_value() ? value.value() : default_value;
}

class FakeStoragePlugin final : public rodaq::StoragePlugin {
public:
    rodaq::Result<void> bind(rodaq::StorageRuntime runtime) override {
        if (stage_ != Stage::Created) {
            return invalid_state();
        }
        runtime_ = std::move(runtime);
        stage_ = Stage::Bound;
        return {};
    }

    rodaq::Result<void> configure(const rodaq::ConfigView& config) override {
        if (stage_ != Stage::Bound) {
            return invalid_state();
        }
        scenario_ = config_string(config, "scenario", "sync");
        event_log_ = config_string(config, "event_log", "");
        callback_delay_us_ = config_int(config, "callback_delay_us", 1000);
        submit_delay_us_ = config_int(config, "submit_delay_us", 0);
        descriptor_scenario_ = config_string(config, "descriptor_scenario", "ready");
        descriptor_stall_after_ = config_int(config, "descriptor_stall_after", -1);
        required_projection_id_ = config_string(config, "required_projection_id", "");
        maximum_inflight_records_ = config_int(config, "maximum_inflight_records", 128);
        maximum_inflight_bytes_ =
            config_int(config, "maximum_inflight_bytes", 128LL * 1024LL * 1024LL);
        stage_ = Stage::Configured;
        return {};
    }

    rodaq::Result<void> open(const rodaq_storage_open_request&) override {
        if (stage_ != Stage::Configured) {
            return invalid_state();
        }
        if (scenario_ == "open_stall") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (scenario_ == "open_failure") {
            return nonstd::make_unexpected(rodaq::Error(
                rodaq::ErrorCode::StorageError, "fake_storage", 0, "configured open failure"));
        }
        stage_ = Stage::Opened;
        return {};
    }

    rodaq::Result<rodaq_storage_capabilities> get_capabilities() override {
        if (scenario_ == "capabilities_stall") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        rodaq_storage_capabilities value{};
        value.struct_size = sizeof(value);
        value.full_fidelity = RODAQ_TRUE;
        value.maximum_payload_bytes = 8U * 1024U * 1024U;
        value.maximum_inflight_records = static_cast<std::uint64_t>(maximum_inflight_records_);
        value.maximum_inflight_bytes = static_cast<std::uint64_t>(maximum_inflight_bytes_);
        value.handoff_progress_timeout_ns = 2'000'000'000;
        value.descriptor_progress_timeout_ns = 2'000'000'000;
        value.flush_progress_timeout_ns = 2'000'000'000;
        value.close_progress_timeout_ns = 2'000'000'000;
        return value;
    }

    rodaq::Result<rodaq_descriptor_readiness_result> ensure_descriptor_ready(
        rodaq_stream_descriptor_handle descriptor) override {
        const std::size_t readiness_call = ++readiness_calls_;
        if (descriptor_scenario_ == "unsupported_dynamic" && readiness_call > 1) {
            rodaq_descriptor_readiness_result value{};
            value.struct_size = sizeof(value);
            value.state = RODAQ_DESCRIPTOR_UNSUPPORTED;
            log_control("unsupported");
            return value;
        }
        const bool stalls = descriptor_scenario_ == "stall" &&
                            (descriptor_stall_after_ < 0 ||
                             readiness_call > static_cast<std::size_t>(descriptor_stall_after_));
        if (stalls || descriptor_scenario_ == "publish_order") {
            log_control("readiness_started descriptor=" + std::to_string(descriptor.value));
        }
        if (descriptor_scenario_ == "publish_order") {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (stalls) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        rodaq_descriptor_readiness_result value{};
        value.struct_size = sizeof(value);
        value.state = descriptor_scenario_ == "unsupported" ? RODAQ_DESCRIPTOR_UNSUPPORTED
                                                            : RODAQ_DESCRIPTOR_READY;
        if (descriptor_scenario_ == "failed" ||
            (descriptor_scenario_ == "failed_dynamic" && readiness_call > 1)) {
            value.state = RODAQ_DESCRIPTOR_FAILED;
            rodaq::assign_rodaq_error(
                RODAQ_STATUS_STORAGE_ERROR, "fake_storage", 0,
                "configured descriptor readiness failure", &value.failure);
        }
        if (descriptor_scenario_ == "require_projection" ||
            descriptor_scenario_ == "canonical_probe" || descriptor_scenario_ == "clock_probe" ||
            descriptor_scenario_ == "publish_order") {
            auto descriptor_view = runtime_.canonical.get_stream_descriptor(descriptor);
            if (!descriptor_view.has_value()) {
                value.state = RODAQ_DESCRIPTOR_FAILED;
                rodaq::assign_rodaq_error(
                    RODAQ_STATUS_NOT_FOUND, "fake_storage", 0,
                    "readiness could not resolve the Canonical Descriptor", &value.failure);
                return value;
            }
            log_control("readiness descriptor=" + std::to_string(descriptor.value));
        }
        if (descriptor_scenario_ == "require_projection") {
            auto projection = runtime_.projection.get(descriptor, required_projection_id_);
            if (!projection.has_value() &&
                projection.error().code() == rodaq::ErrorCode::NotFound) {
                value.state = RODAQ_DESCRIPTOR_UNSUPPORTED;
                return value;
            }
            if (!projection.has_value()) {
                value.state = RODAQ_DESCRIPTOR_FAILED;
                rodaq::assign_rodaq_error(
                    RODAQ_STATUS_STORAGE_ERROR, "fake_storage", 0,
                    "readiness could not resolve the required Projection", &value.failure);
                return value;
            }
            log_control("readiness projection=" + required_projection_id_);
        }
        if (descriptor_scenario_ == "clock_probe") {
            auto descriptor_view = runtime_.canonical.get_stream_descriptor(descriptor);
            if (!descriptor_view.has_value()) {
                value.state = RODAQ_DESCRIPTOR_FAILED;
                return value;
            }
            auto keys = runtime_.canonical.get_timestamp_key_set(
                descriptor_view.value().timestamp_key_set_id);
            if (!keys.has_value() || keys.value().key_count == 0) {
                value.state = RODAQ_DESCRIPTOR_FAILED;
                return value;
            }
            const rodaq_clock_handle ingress_clock = keys.value().keys[0].clock;
            auto first = runtime_.clock_correlation.anchor(ingress_clock);
            auto second = runtime_.clock_correlation.anchor(ingress_clock);
            auto unknown = runtime_.clock_correlation.anchor(
                rodaq_clock_handle{ingress_clock.value + 1'000'000U});
            if (!first.has_value() || !second.has_value() ||
                first.value().clock_reading != second.value().clock_reading ||
                first.value().unix_anchor_ns != second.value().unix_anchor_ns ||
                unknown.has_value() || unknown.error().code() != rodaq::ErrorCode::NotFound) {
                value.state = RODAQ_DESCRIPTOR_FAILED;
                rodaq::assign_rodaq_error(
                    RODAQ_STATUS_STORAGE_ERROR, "fake_storage", 0, "clock correlation probe failed",
                    &value.failure);
                return value;
            }
            log_control(
                "clock_probe ingress=" + std::to_string(ingress_clock.value) +
                " reading=" + std::to_string(first.value().clock_reading));
        }
        return value;
    }

    rodaq::Result<rodaq_storage_submit_result> submit(
        const rodaq_storage_submission& submission) override {
        if (stage_ != Stage::Opened) {
            return invalid_state_value();
        }
        log_control("submit");
        if (scenario_ == "submit_error" || scenario_ == "delayed_submit_error") {
            if (scenario_ == "delayed_submit_error") {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return nonstd::make_unexpected(rodaq::Error(
                rodaq::ErrorCode::StorageError, "fake_storage", 0, "configured submit failure"));
        }
        log(submission);
        if (submit_delay_us_ > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(submit_delay_us_));
        }
        if (scenario_ == "storage_stall" || scenario_ == "stall_then_sync") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (scenario_ == "background_fatal") {
            schedule_failure();
        }

        if (scenario_ == "same_stack") {
            runtime_.storage.payload_consumed(submission.operation);
        }

        rodaq_storage_submit_result result{};
        result.struct_size = sizeof(result);
        result.payload_state = RODAQ_STORAGE_PAYLOAD_PENDING;
        result.persistence_state = RODAQ_STORAGE_PERSISTENCE_PENDING;

        if (scenario_ == "sync" || scenario_ == "stall_then_sync") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
            result.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
            return result;
        }
        if (scenario_ == "sync_payload_async_persistence") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
            schedule_persistence(submission.operation, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
            return result;
        }
        if (scenario_ == "async_payload_sync_persistence") {
            result.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
            schedule_payload(submission.operation);
            return result;
        }
        if (scenario_ == "async_payload_async_persistence") {
            schedule_dual_completion(
                submission.operation, true, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
            return result;
        }
        if (scenario_ == "persistence_first") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_PENDING;
            schedule_dual_completion(
                submission.operation, false, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
            return result;
        }
        if (scenario_ == "persistence_failed") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
            schedule_persistence(submission.operation, RODAQ_STORAGE_PERSISTENCE_FAILED);
            return result;
        }
        if (scenario_ == "persistence_unknown") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
            schedule_persistence(submission.operation, RODAQ_STORAGE_PERSISTENCE_UNKNOWN);
            return result;
        }
        if (scenario_ == "missing_callback") {
            return result;
        }
        if (scenario_ == "callback_after_close") {
            return result;
        }
        if (scenario_ == "duplicate_payload") {
            result.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
            schedule_payload(submission.operation);
            schedule_payload(submission.operation);
            return result;
        }
        if (scenario_ == "duplicate_persistence") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
            schedule_persistence(submission.operation, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
            schedule_persistence(submission.operation, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
            return result;
        }
        if (scenario_ == "sync_persistence_callback") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
            result.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
            schedule_persistence(submission.operation, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
            return result;
        }
        if (scenario_ == "unknown_operation") {
            result.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
            schedule_payload(rodaq_storage_operation{submission.operation.value + 1'000'000U});
            return result;
        }
        if (scenario_ == "missing_diagnostic") {
            workers_.emplace_back([this, operation = submission.operation] {
                std::this_thread::sleep_for(std::chrono::microseconds(callback_delay_us_));
                runtime_.storage.persistence_completed(
                    operation, RODAQ_STORAGE_PERSISTENCE_FAILED, nullptr);
            });
            return result;
        }
        if (scenario_ == "invalid_persistence_state") {
            workers_.emplace_back([this, operation = submission.operation] {
                std::this_thread::sleep_for(std::chrono::microseconds(callback_delay_us_));
                rodaq_error error{};
                rodaq::assign_rodaq_error(
                    RODAQ_STATUS_STORAGE_ERROR, "fake_storage", 0, "invalid persistence state",
                    &error);
                runtime_.storage.persistence_completed(
                    operation, static_cast<rodaq_storage_persistence_state>(0), &error);
            });
            return result;
        }
        if (scenario_ == "malformed_callback_diagnostic") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
            workers_.emplace_back([this, operation = submission.operation] {
                rodaq_error error{};
                rodaq::assign_rodaq_error(
                    RODAQ_STATUS_STORAGE_ERROR, "fake_storage", 0,
                    "configured malformed persistence failure", &error);
                error.reserved[0] = 1;
                runtime_.storage.persistence_completed(
                    operation, RODAQ_STORAGE_PERSISTENCE_FAILED, &error);
            });
            return result;
        }
        if (scenario_ == "callback_before_error") {
            std::atomic<bool> callback_done{false};
            workers_.emplace_back([this, operation = submission.operation, &callback_done] {
                runtime_.storage.payload_consumed(operation);
                callback_done.store(true);
            });
            while (!callback_done.load()) {
                std::this_thread::yield();
            }
            join_workers();
            return nonstd::make_unexpected(rodaq::Error(
                rodaq::ErrorCode::StorageError, "fake_storage", 0,
                "callback before configured submit failure"));
        }
        if (scenario_ == "early_cross_thread") {
            result.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
            std::atomic<bool> callback_done{false};
            workers_.emplace_back([this, operation = submission.operation, &callback_done] {
                runtime_.storage.payload_consumed(operation);
                callback_done.store(true);
            });
            while (!callback_done.load()) {
                std::this_thread::yield();
            }
            join_workers();
            return result;
        }

        schedule_dual_completion(submission.operation, true, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
        return result;
    }

    rodaq::Result<void> flush() override {
        if (stage_ != Stage::Opened) {
            return invalid_state();
        }
        if (scenario_ == "flush_stall") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (scenario_ == "flush_failure" || scenario_ == "flush_and_close_failure") {
            return nonstd::make_unexpected(rodaq::Error(
                rodaq::ErrorCode::StorageError, "fake_storage", 0, "configured flush failure"));
        }
        return {};
    }

    rodaq::Result<void> close(rodaq_storage_close_disposition disposition) override {
        log_control("close disposition=" + std::to_string(disposition));
        if (stage_ != Stage::Opened) {
            return invalid_state();
        }
        if (scenario_ == "close_stall") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        join_workers();
        stage_ = Stage::Closed;
        if (scenario_ == "close_failure" || scenario_ == "flush_and_close_failure") {
            return nonstd::make_unexpected(rodaq::Error(
                rodaq::ErrorCode::StorageError, "fake_storage", 0, "configured close failure"));
        }
        if (scenario_ == "callback_after_close" && !last_operation_.has_value()) {
            return {};
        }
        if (scenario_ == "callback_after_close") {
            workers_.emplace_back([this, operation = last_operation_.value()] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                runtime_.storage.payload_consumed(operation);
            });
        }
        return {};
    }

    ~FakeStoragePlugin() override {
        join_workers();
        log_control("destroy");
    }

private:
    enum class Stage {
        Created,
        Bound,
        Configured,
        Opened,
        Closed,
    };

    void log(const rodaq_storage_submission& submission) {
        last_operation_ = submission.operation;
        if (event_log_.empty()) {
            return;
        }
        std::uint64_t checksum = 0;
        for (std::uint64_t index = 0; index < submission.payload.size; ++index) {
            checksum = checksum * 131U + submission.payload.data[index];
        }
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::ofstream output(event_log_, std::ios::app);
        output << "record stream=" << submission.stream.value
               << " descriptor=" << submission.descriptor.value
               << " sequence=" << submission.acceptance_sequence
               << " bytes=" << submission.payload.size << " checksum=" << checksum
               << " timestamp_count=" << submission.timestamp_value_count << " ingress_present="
               << (submission.timestamp_value_count > 0 &&
                           submission.timestamp_values[0].present == RODAQ_TRUE
                       ? 1
                       : 0);
        if (submission.timestamp_value_count > 1) {
            output << " sample_present="
                   << (submission.timestamp_values[1].present == RODAQ_TRUE ? 1 : 0)
                   << " sample_reading=" << submission.timestamp_values[1].reading
                   << " ingress_reading=" << submission.timestamp_values[0].reading;
        }
        output << '\n';
    }

    void log_control(const std::string& event) {
        if (event_log_.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::ofstream output(event_log_, std::ios::app);
        output << "control storage_" << event << '\n';
    }

    void schedule_payload(rodaq_storage_operation operation) {
        workers_.emplace_back([this, operation] {
            std::this_thread::sleep_for(std::chrono::microseconds(callback_delay_us_));
            runtime_.storage.payload_consumed(operation);
        });
    }

    void schedule_dual_completion(
        rodaq_storage_operation operation, bool payload_first,
        rodaq_storage_persistence_state persistence_state) {
        workers_.emplace_back([this, operation, payload_first, persistence_state] {
            std::this_thread::sleep_for(std::chrono::microseconds(callback_delay_us_));
            auto payload = [this, operation] { runtime_.storage.payload_consumed(operation); };
            auto persistence = [this, operation, persistence_state] {
                rodaq_error error{};
                if (persistence_state != RODAQ_STORAGE_PERSISTENCE_SUCCEEDED) {
                    rodaq::assign_rodaq_error(
                        RODAQ_STATUS_STORAGE_ERROR, "fake_storage", 0,
                        "configured persistence failure", &error);
                    runtime_.storage.persistence_completed(operation, persistence_state, &error);
                } else {
                    runtime_.storage.persistence_completed(operation, persistence_state, nullptr);
                }
            };
            if (payload_first) {
                payload();
                persistence();
            } else {
                persistence();
                payload();
            }
        });
    }

    void schedule_persistence(
        rodaq_storage_operation operation, rodaq_storage_persistence_state state) {
        workers_.emplace_back([this, operation, state] {
            std::this_thread::sleep_for(std::chrono::microseconds(callback_delay_us_));
            rodaq_error error{};
            if (state != RODAQ_STORAGE_PERSISTENCE_SUCCEEDED) {
                rodaq::assign_rodaq_error(
                    RODAQ_STATUS_STORAGE_ERROR, "fake_storage", 0, "configured persistence failure",
                    &error);
                runtime_.storage.persistence_completed(operation, state, &error);
            } else {
                runtime_.storage.persistence_completed(operation, state, nullptr);
            }
        });
    }

    void schedule_failure() {
        workers_.emplace_back([this] {
            std::this_thread::sleep_for(std::chrono::microseconds(callback_delay_us_));
            rodaq_error error{};
            rodaq::assign_rodaq_error(
                RODAQ_STATUS_STORAGE_ERROR, "fake_storage", 0, "configured background failure",
                &error);
            runtime_.storage.report_failure(&error);
        });
    }

    void join_workers() {
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

    static rodaq::Result<void> invalid_state() {
        return nonstd::make_unexpected(
            rodaq::Error(rodaq::ErrorCode::InvalidState, "fake_storage", 0, "invalid state"));
    }

    static rodaq::Result<rodaq_storage_submit_result> invalid_state_value() {
        return nonstd::make_unexpected(
            rodaq::Error(rodaq::ErrorCode::InvalidState, "fake_storage", 0, "invalid state"));
    }

    rodaq::StorageRuntime runtime_;
    Stage stage_ = Stage::Created;
    std::string scenario_ = "sync";
    std::string descriptor_scenario_ = "ready";
    std::int64_t descriptor_stall_after_ = -1;
    std::string required_projection_id_;
    std::string event_log_;
    std::int64_t callback_delay_us_ = 1000;
    std::int64_t submit_delay_us_ = 0;
    std::int64_t maximum_inflight_records_ = 128;
    std::int64_t maximum_inflight_bytes_ = 128LL * 1024LL * 1024LL;
    std::mutex log_mutex_;
    std::vector<std::thread> workers_;
    std::optional<rodaq_storage_operation> last_operation_;
    std::atomic<std::size_t> readiness_calls_{0};
};

}  // namespace

RODAQ_EXPORT_STORAGE_PLUGIN(
    FakeStoragePlugin, rodaq::plugin::generated::rodaq_fake_storage_plugin::descriptor)
