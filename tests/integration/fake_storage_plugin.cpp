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
    auto child = view.Find(key);
    if (!child.has_value()) {
        return default_value;
    }
    auto value = child.value().AsString();
    return value.has_value() ? std::string(value.value()) : std::move(default_value);
}

std::int64_t config_int(
    const rodaq::ConfigView& view, std::string_view key, std::int64_t default_value) {
    auto child = view.Find(key);
    if (!child.has_value()) {
        return default_value;
    }
    auto value = child.value().AsInt64();
    return value.has_value() ? value.value() : default_value;
}

class FakeStoragePlugin final : public rodaq::StoragePlugin {
public:
    rodaq::Result<void> Bind(rodaq::StorageRuntime runtime) override {
        if (stage_ != Stage::Created) {
            return invalid_state();
        }
        runtime_ = std::move(runtime);
        stage_ = Stage::Bound;
        return {};
    }

    rodaq::Result<void> Configure(const rodaq::ConfigView& config) override {
        if (stage_ != Stage::Bound) {
            return invalid_state();
        }
        scenario_ = config_string(config, "scenario", "sync");
        event_log_ = config_string(config, "event_log", "");
        callback_delay_us_ = config_int(config, "callback_delay_us", 1000);
        stage_ = Stage::Configured;
        return {};
    }

    rodaq::Result<void> Open(const rodaq_storage_open_request&) override {
        if (stage_ != Stage::Configured) {
            return invalid_state();
        }
        stage_ = Stage::Opened;
        return {};
    }

    rodaq::Result<rodaq_storage_capabilities> GetCapabilities() override {
        rodaq_storage_capabilities value{};
        value.struct_size = sizeof(value);
        value.full_fidelity = RODAQ_TRUE;
        value.maximum_payload_bytes = 8U * 1024U * 1024U;
        value.maximum_inflight_records = 128;
        value.maximum_inflight_bytes = 128U * 1024U * 1024U;
        value.handoff_progress_timeout_ns = 2'000'000'000;
        value.descriptor_progress_timeout_ns = 2'000'000'000;
        value.flush_progress_timeout_ns = 2'000'000'000;
        value.close_progress_timeout_ns = 2'000'000'000;
        return value;
    }

    rodaq::Result<rodaq_descriptor_readiness_result> EnsureDescriptorReady(
        rodaq_stream_descriptor_handle) override {
        rodaq_descriptor_readiness_result value{};
        value.struct_size = sizeof(value);
        value.state = RODAQ_DESCRIPTOR_READY;
        return value;
    }

    rodaq::Result<rodaq_storage_submit_result> Submit(
        const rodaq_storage_submission& submission) override {
        if (stage_ != Stage::Opened) {
            return invalid_state_value();
        }
        if (scenario_ == "submit_error") {
            return nonstd::make_unexpected(rodaq::Error(
                rodaq::ErrorCode::StorageError, "fake_storage", 0, "configured submit failure"));
        }
        Log(submission);

        if (scenario_ == "same_stack") {
            runtime_.storage.PayloadConsumed(submission.operation);
        }

        rodaq_storage_submit_result result{};
        result.struct_size = sizeof(result);
        result.payload_state = RODAQ_STORAGE_PAYLOAD_PENDING;
        result.persistence_state = RODAQ_STORAGE_PERSISTENCE_PENDING;

        if (scenario_ == "sync") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
            result.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
            return result;
        }
        if (scenario_ == "sync_payload_async_persistence") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
            SchedulePersistence(submission.operation, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
            return result;
        }
        if (scenario_ == "async_payload_sync_persistence") {
            result.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
            SchedulePayload(submission.operation);
            return result;
        }
        if (scenario_ == "async_payload_async_persistence") {
            ScheduleDualCompletion(submission.operation, true, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
            return result;
        }
        if (scenario_ == "persistence_first") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_PENDING;
            ScheduleDualCompletion(
                submission.operation, false, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
            return result;
        }
        if (scenario_ == "persistence_failed") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
            SchedulePersistence(submission.operation, RODAQ_STORAGE_PERSISTENCE_FAILED);
            return result;
        }
        if (scenario_ == "persistence_unknown") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
            SchedulePersistence(submission.operation, RODAQ_STORAGE_PERSISTENCE_UNKNOWN);
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
            SchedulePayload(submission.operation);
            SchedulePayload(submission.operation);
            return result;
        }
        if (scenario_ == "duplicate_persistence") {
            result.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
            SchedulePersistence(submission.operation, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
            SchedulePersistence(submission.operation, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
            return result;
        }
        if (scenario_ == "callback_before_error") {
            std::atomic<bool> callback_done{false};
            workers_.emplace_back([this, operation = submission.operation, &callback_done] {
                runtime_.storage.PayloadConsumed(operation);
                callback_done.store(true);
            });
            while (!callback_done.load()) {
                std::this_thread::yield();
            }
            JoinWorkers();
            return nonstd::make_unexpected(rodaq::Error(
                rodaq::ErrorCode::StorageError, "fake_storage", 0,
                "callback before configured submit failure"));
        }
        if (scenario_ == "early_cross_thread") {
            result.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
            std::atomic<bool> callback_done{false};
            workers_.emplace_back([this, operation = submission.operation, &callback_done] {
                runtime_.storage.PayloadConsumed(operation);
                callback_done.store(true);
            });
            while (!callback_done.load()) {
                std::this_thread::yield();
            }
            JoinWorkers();
            return result;
        }

        ScheduleDualCompletion(submission.operation, true, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED);
        return result;
    }

    rodaq::Result<void> Flush() override {
        return stage_ == Stage::Opened ? rodaq::Result<void>() : invalid_state();
    }

    rodaq::Result<void> Close(rodaq_storage_close_disposition) override {
        if (stage_ != Stage::Opened) {
            return invalid_state();
        }
        JoinWorkers();
        stage_ = Stage::Closed;
        if (scenario_ == "callback_after_close" && !last_operation_.has_value()) {
            return {};
        }
        if (scenario_ == "callback_after_close") {
            workers_.emplace_back([this, operation = last_operation_.value()] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                runtime_.storage.PayloadConsumed(operation);
            });
        }
        return {};
    }

    ~FakeStoragePlugin() override { JoinWorkers(); }

private:
    enum class Stage {
        Created,
        Bound,
        Configured,
        Opened,
        Closed,
    };

    void Log(const rodaq_storage_submission& submission) {
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
               << " bytes=" << submission.payload.size << " checksum=" << checksum << '\n';
    }

    void SchedulePayload(rodaq_storage_operation operation) {
        workers_.emplace_back([this, operation] {
            std::this_thread::sleep_for(std::chrono::microseconds(callback_delay_us_));
            runtime_.storage.PayloadConsumed(operation);
        });
    }

    void ScheduleDualCompletion(
        rodaq_storage_operation operation, bool payload_first,
        rodaq_storage_persistence_state persistence_state) {
        workers_.emplace_back([this, operation, payload_first, persistence_state] {
            std::this_thread::sleep_for(std::chrono::microseconds(callback_delay_us_));
            auto payload = [this, operation] { runtime_.storage.PayloadConsumed(operation); };
            auto persistence = [this, operation, persistence_state] {
                rodaq_error error{};
                if (persistence_state != RODAQ_STORAGE_PERSISTENCE_SUCCEEDED) {
                    rodaq::assign_rodaq_error(
                        RODAQ_STATUS_STORAGE_ERROR, "fake_storage", 0,
                        "configured persistence failure", &error);
                    runtime_.storage.PersistenceCompleted(operation, persistence_state, &error);
                } else {
                    runtime_.storage.PersistenceCompleted(operation, persistence_state, nullptr);
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

    void SchedulePersistence(
        rodaq_storage_operation operation, rodaq_storage_persistence_state state) {
        workers_.emplace_back([this, operation, state] {
            std::this_thread::sleep_for(std::chrono::microseconds(callback_delay_us_));
            rodaq_error error{};
            if (state != RODAQ_STORAGE_PERSISTENCE_SUCCEEDED) {
                rodaq::assign_rodaq_error(
                    RODAQ_STATUS_STORAGE_ERROR, "fake_storage", 0, "configured persistence failure",
                    &error);
                runtime_.storage.PersistenceCompleted(operation, state, &error);
            } else {
                runtime_.storage.PersistenceCompleted(operation, state, nullptr);
            }
        });
    }

    void JoinWorkers() {
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
    std::string event_log_;
    std::int64_t callback_delay_us_ = 1000;
    std::mutex log_mutex_;
    std::vector<std::thread> workers_;
    std::optional<rodaq_storage_operation> last_operation_;
};

}  // namespace

RODAQ_EXPORT_STORAGE_PLUGIN(
    FakeStoragePlugin, rodaq::plugin::generated::rodaq_fake_storage_plugin::descriptor)
