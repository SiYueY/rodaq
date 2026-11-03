#include <rodaq/plugin/acquisition_plugin.hpp>

#include "rodaq_fake_acquisition_plugin_metadata.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct Anchor {
    std::vector<std::uint8_t> bytes;
};

void release_anchor(void* context) noexcept { delete static_cast<Anchor*>(context); }

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

class FakeAcquisitionPlugin final : public rodaq::AcquisitionPlugin {
public:
    rodaq::Result<void> Bind(rodaq::AcquisitionRuntime runtime) override {
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
        stream_count_ = static_cast<std::size_t>(config_int(config, "streams", 1));
        records_per_stream_ = static_cast<std::size_t>(config_int(config, "records_per_stream", 1));
        producer_threads_ = static_cast<std::size_t>(config_int(config, "producer_threads", 1));
        payload_size_ = static_cast<std::size_t>(config_int(config, "payload_size", 16));
        payload_mode_ = config_string(config, "payload_mode", "copy");
        event_log_ = config_string(config, "event_log", "");
        if (stream_count_ == 0 || records_per_stream_ == 0 || producer_threads_ == 0 ||
            (payload_mode_ != "copy" && payload_mode_ != "adopt" && payload_mode_ != "retain")) {
            return invalid_argument();
        }
        stage_ = Stage::Configured;
        return {};
    }

    rodaq::Result<void> Start() override {
        if (stage_ != Stage::Configured) {
            return invalid_state();
        }
        auto source = runtime_.acquisition.RegisterSource(rodaq_source_registration{
            RODAQ_SOURCE_REGISTRATION_MIN_STRUCT_SIZE,
            0,
            rodaq_bytes_view{
                reinterpret_cast<const std::uint8_t*>("fake-source"), sizeof("fake-source") - 1U},
        });
        if (!source.has_value()) {
            return fail(source.error());
        }
        source_ = source.value();
        auto clock = runtime_.acquisition.RegisterClock(rodaq_clock_registration{
            RODAQ_CLOCK_REGISTRATION_MIN_STRUCT_SIZE,
            0,
            rodaq_bytes_view{
                reinterpret_cast<const std::uint8_t*>("fake-clock"), sizeof("fake-clock") - 1U},
            1,
            1'000'000'000,
            fixture_string("monotonic"),
        });
        if (!clock.has_value()) {
            return fail(clock.error());
        }
        auto schema = runtime_.acquisition.RegisterSchema(rodaq_schema_registration{
            RODAQ_SCHEMA_REGISTRATION_MIN_STRUCT_SIZE,
            RODAQ_FALSE,
            0,
            fixture_string("raw"),
            {},
            rodaq_bytes_view{
                reinterpret_cast<const std::uint8_t*>("schema"), sizeof("schema") - 1U},
        });
        if (!schema.has_value()) {
            return fail(schema.error());
        }

        streams_.clear();
        descriptors_.clear();
        streams_.reserve(stream_count_);
        descriptors_.reserve(stream_count_);
        for (std::size_t index = 0; index < stream_count_; ++index) {
            const std::string native = "stream-" + std::to_string(index);
            auto stream = runtime_.acquisition.RegisterStream(rodaq_stream_registration{
                RODAQ_STREAM_REGISTRATION_MIN_STRUCT_SIZE,
                0,
                rodaq_bytes_view{
                    reinterpret_cast<const std::uint8_t*>(native.data()), native.size()},
            });
            if (!stream.has_value()) {
                return fail(stream.error());
            }
            const std::string name = "fake/" + std::to_string(index);
            const rodaq_timestamp_key timestamp_key{
                fixture_string("sample_time"),
                clock.value(),
            };
            auto descriptor =
                runtime_.acquisition.PublishDescriptor(rodaq_stream_descriptor_registration{
                    RODAQ_STREAM_DESCRIPTOR_REGISTRATION_MIN_STRUCT_SIZE,
                    RODAQ_TRUE,
                    0,
                    stream.value(),
                    rodaq_string_view{name.data(), name.size()},
                    schema.value(),
                    fixture_string("raw"),
                    &timestamp_key,
                    1,
                    0,
                });
            if (!descriptor.has_value()) {
                return fail(descriptor.error());
            }
            streams_.push_back(stream.value());
            descriptors_.push_back(descriptor.value());
        }

        stopping_.store(false);
        for (std::size_t producer = 0; producer < producer_threads_; ++producer) {
            producers_.emplace_back([this, producer]() { Produce(producer); });
        }
        stage_ = Stage::Started;
        return {};
    }

    rodaq::Result<void> Stop() override {
        if (stage_ == Stage::Stopped) {
            return {};
        }
        if (stage_ != Stage::Started) {
            return invalid_state();
        }
        Quiesce();
        stage_ = Stage::Stopped;
        return {};
    }

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

    rodaq::Result<void> QuiescePlugin() override {
        Quiesce();
        return {};
    }

private:
    enum class Stage {
        Created,
        Bound,
        Configured,
        Started,
        Stopped,
    };

    void Quiesce() {
        stopping_.store(true);
        for (std::thread& producer : producers_) {
            if (producer.joinable()) {
                producer.join();
            }
        }
        producers_.clear();
    }

    void Produce(std::size_t producer_index) {
        while (!stopping_.load()) {
            const auto lifecycle = runtime_.common.LifecycleSnapshot();
            if (lifecycle.has_value() &&
                lifecycle.value().normal_acquisition_allowed == RODAQ_TRUE) {
                break;
            }
            if (lifecycle.has_value() && lifecycle.value().state == RODAQ_RUNTIME_FAILED) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        for (std::size_t stream_index = 0; stream_index < streams_.size() && !stopping_.load();
             ++stream_index) {
            for (std::size_t record_index = 0;
                 record_index < records_per_stream_ && !stopping_.load(); ++record_index) {
                SubmitOne(producer_index, stream_index, record_index);
            }
        }
    }

    void SubmitOne(std::size_t producer_index, std::size_t stream_index, std::size_t record_index) {
        std::vector<std::uint8_t> source(payload_size_);
        for (std::size_t index = 0; index < source.size(); ++index) {
            source[index] =
                static_cast<std::uint8_t>(producer_index + stream_index + record_index + index);
        }

        Anchor* anchor = nullptr;
        const std::uint8_t* data = source.data();
        void* owner_context = nullptr;
        void (*release)(void*) noexcept = nullptr;
        rodaq_payload_mode mode = RODAQ_PAYLOAD_COPY;
        if (payload_mode_ == "adopt" || payload_mode_ == "retain") {
            anchor = new Anchor{source};
            data = anchor->bytes.data();
            owner_context = anchor;
            release = &release_anchor;
            mode = payload_mode_ == "adopt" ? RODAQ_PAYLOAD_ADOPT : RODAQ_PAYLOAD_RETAIN;
        }

        const std::int64_t reading = static_cast<std::int64_t>(record_index + 1U);
        const rodaq_timestamp_value timestamp{RODAQ_TRUE, 0, reading};
        rodaq_record_submission submission{};
        submission.struct_size = sizeof(submission);
        submission.stream = streams_[stream_index];
        submission.descriptor = descriptors_[stream_index];
        submission.source_present = RODAQ_TRUE;
        submission.source.value = source_.value_or(rodaq_source_handle{}).value;
        submission.timestamp_values = &timestamp;
        submission.timestamp_value_count = 1;
        submission.payload.mode = mode;
        submission.payload.data = data;
        submission.payload.size = source.size();
        submission.payload.owner_context = owner_context;
        submission.payload.release = release;

        auto result = runtime_.acquisition.Submit(submission);
        if (!result.has_value()) {
            if (anchor != nullptr) {
                delete anchor;
            }
            return;
        }
        if (anchor != nullptr &&
            result.value().payload_disposition == RODAQ_PAYLOAD_DISPOSITION_CALLER_RESPONSIBLE) {
            delete anchor;
        }
        LogResult(submission, result.value());
    }

    void LogResult(const rodaq_record_submission&, const rodaq_acquisition_submit_result& result) {
        if (event_log_.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::ofstream output(event_log_, std::ios::app);
        output << "submit outcome=" << result.outcome
               << " disposition=" << result.payload_disposition << '\n';
    }

    static rodaq_string_view fixture_string(std::string_view value) noexcept {
        return rodaq_string_view{value.data(), value.size()};
    }

    static rodaq::Result<void> invalid_state() {
        return nonstd::make_unexpected(
            rodaq::Error(rodaq::ErrorCode::InvalidState, "fake_acquisition", 0, "invalid state"));
    }

    static rodaq::Result<void> invalid_argument() {
        return nonstd::make_unexpected(rodaq::Error(
            rodaq::ErrorCode::InvalidArgument, "fake_acquisition", 0, "invalid config"));
    }

    static rodaq::Result<void> fail(const rodaq::Error& error) {
        return nonstd::make_unexpected(error);
    }

    rodaq::AcquisitionRuntime runtime_;
    Stage stage_ = Stage::Created;
    std::size_t stream_count_ = 1;
    std::size_t records_per_stream_ = 1;
    std::size_t producer_threads_ = 1;
    std::size_t payload_size_ = 16;
    std::string payload_mode_ = "copy";
    std::string event_log_;
    std::mutex log_mutex_;
    std::atomic<bool> stopping_{false};
    std::vector<std::thread> producers_;
    std::vector<rodaq_stream_handle> streams_;
    std::vector<rodaq_stream_descriptor_handle> descriptors_;
    std::optional<rodaq_source_handle> source_;
};

}  // namespace

RODAQ_EXPORT_ACQUISITION_PLUGIN(
    FakeAcquisitionPlugin, rodaq::plugin::generated::rodaq_fake_acquisition_plugin::descriptor)
