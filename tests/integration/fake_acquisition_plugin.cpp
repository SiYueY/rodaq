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
        descriptor_change_after_ = config_int(config, "descriptor_change_after", -1);
        binding_scenario_ = config_string(config, "binding_scenario", "");
        lifecycle_scenario_ = config_string(config, "lifecycle_scenario", "");
        pause_before_switch_ = config_int(config, "pause_before_switch", 0) != 0;
        stale_handle_file_ = config_string(config, "stale_handle_file", "");
        projection_id_ = config_string(config, "projection_id", "");
        projection_before_start_ = config_int(config, "projection_before_start", 0) != 0;
        projection_delay_after_ = config_int(config, "projection_delay_after", -1);
        projection_registration_mode_ = config_string(config, "projection_registration_mode", "");
        timestamp_scenario_ = config_string(config, "timestamp_scenario", "");
        timestamp_key_count_ =
            static_cast<std::size_t>(config_int(config, "timestamp_key_count", 1));
        event_log_ = config_string(config, "event_log", "");
        LogControl(
            "configure streams=" + std::to_string(stream_count_) +
            " records=" + std::to_string(records_per_stream_) +
            " producers=" + std::to_string(producer_threads_) + " payload_mode=" + payload_mode_);
        if (stream_count_ == 0 || records_per_stream_ == 0 || producer_threads_ == 0 ||
            timestamp_key_count_ == 0 ||
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
        if (lifecycle_scenario_ == "start_stall") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
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
        clock_ = clock.value();
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
        schema_ = schema.value();

        streams_.clear();
        descriptors_.clear();
        switched_.assign(stream_count_, false);
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
            std::string descriptor_name = "fake/0";
            auto descriptor = PublishDescriptor(stream.value(), descriptor_name);
            if (!descriptor.has_value()) {
                return fail(descriptor.error());
            }
            if (projection_before_start_) {
                auto projection = RegisterProjection(descriptor.value());
                if (!projection.has_value()) {
                    return fail(projection.error());
                }
                if (projection_registration_mode_ == "same") {
                    auto duplicate = RegisterProjection(descriptor.value());
                    if (!duplicate.has_value()) {
                        return fail(duplicate.error());
                    }
                } else if (projection_registration_mode_ == "conflict") {
                    auto conflicting = RegisterProjection(descriptor.value(), "-different");
                    if (!conflicting.has_value()) {
                        return fail(conflicting.error());
                    }
                }
            }
            descriptor_name.assign("mutated-after-publication");
            auto views = ValidatePublishedViews(stream.value(), descriptor.value(), "fake/0");
            if (!views.has_value()) {
                return fail(views.error());
            }
            streams_.push_back(stream.value());
            descriptors_.push_back(descriptor.value());
        }
        auto stale_handles = ValidateOrRecordStaleHandles();
        if (!stale_handles.has_value()) {
            return fail(stale_handles.error());
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
        if (lifecycle_scenario_ == "stop_stall") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        Quiesce();
        stage_ = Stage::Stopped;
        if (lifecycle_scenario_ == "stop_failure") {
            return fail(rodaq::Error(
                rodaq::ErrorCode::Unavailable, "fake_acquisition", 0, "configured stop failure"));
        }
        return {};
    }

    rodaq::Result<void> PrepareBindingSwitch(
        rodaq_binding_transition transition, rodaq_stream_handle stream,
        rodaq_stream_descriptor_handle old_descriptor,
        rodaq_stream_descriptor_handle new_descriptor) override {
        static_cast<void>(stream);
        LogControl(
            "prepare transition=" + std::to_string(transition.value) + " old=" +
            std::to_string(old_descriptor.value) + " new=" + std::to_string(new_descriptor.value));
        if ((binding_scenario_ == "prepare_failure_initial" && old_descriptor.value == 0) ||
            (binding_scenario_ == "prepare_failure_switch" && old_descriptor.value != 0)) {
            return fail(rodaq::Error(
                rodaq::ErrorCode::Unavailable, "fake_acquisition", 0,
                "configured binding prepare failure"));
        }
        return {};
    }

    rodaq::Result<void> CommitBindingSwitch(rodaq_binding_transition transition) override {
        LogControl("commit transition=" + std::to_string(transition.value));
        if (binding_scenario_ == "commit_failure_switch" && commit_count_.fetch_add(1) != 0) {
            return fail(rodaq::Error(
                rodaq::ErrorCode::Unavailable, "fake_acquisition", 0,
                "configured binding commit failure"));
        }
        if (binding_scenario_ != "commit_failure_switch") {
            commit_count_.fetch_add(1);
        }
        return {};
    }

    rodaq::Result<void> AbortBindingSwitch(
        rodaq_binding_transition transition, rodaq_binding_abort_disposition disposition) override {
        LogControl(
            "abort transition=" + std::to_string(transition.value) +
            " disposition=" + std::to_string(disposition));
        return {};
    }

    rodaq::Result<void> QuiesceStream(rodaq_stream_handle) override { return {}; }

    rodaq::Result<void> QuiescePlugin() override {
        LogControl("quiesce");
        if (lifecycle_scenario_ == "quiesce_failure") {
            return fail(rodaq::Error(
                rodaq::ErrorCode::Unavailable, "fake_acquisition", 0,
                "configured quiesce failure"));
        }
        if (lifecycle_scenario_ == "quiesce_stall") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
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
        if (projection_delay_after_ >= 0 &&
            record_index == static_cast<std::size_t>(projection_delay_after_)) {
            auto projection = RegisterProjection(descriptors_[stream_index]);
            if (!projection.has_value()) {
                return;
            }
        }
        rodaq_stream_descriptor_handle descriptor{};
        {
            std::lock_guard<std::mutex> lock(switch_mutex_);
            if (descriptor_change_after_ >= 0 &&
                record_index == static_cast<std::size_t>(descriptor_change_after_) &&
                !switched_[stream_index]) {
                switched_[stream_index] = true;
                if (pause_before_switch_) {
                    const auto paused = runtime_.acquisition.ReportStream(
                        streams_[stream_index], RODAQ_AVAILABILITY_UNAVAILABLE, nullptr);
                    if (!paused.has_value()) {
                        return;
                    }
                }
                auto switched_descriptor =
                    PublishDescriptor(streams_[stream_index], "fake/switched");
                if (!switched_descriptor.has_value()) {
                    return;
                }
                descriptors_[stream_index] = switched_descriptor.value();
                if (projection_before_start_) {
                    auto projection = RegisterProjection(descriptors_[stream_index]);
                    if (!projection.has_value()) {
                        return;
                    }
                }
            }
            descriptor = descriptors_[stream_index];
        }
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

        std::vector<rodaq_timestamp_value> timestamps(timestamp_key_count_);
        for (std::size_t slot = 0; slot < timestamps.size(); ++slot) {
            timestamps[slot] = rodaq_timestamp_value{
                RODAQ_TRUE, 0, static_cast<std::int64_t>(record_index + slot + 1U)};
        }
        rodaq_record_submission submission{};
        submission.struct_size = sizeof(submission);
        submission.stream = streams_[stream_index];
        submission.descriptor = descriptor;
        submission.source_present = RODAQ_TRUE;
        submission.source.value = source_.value_or(rodaq_source_handle{}).value;
        submission.timestamp_values = timestamps.data();
        submission.timestamp_value_count = static_cast<std::uint32_t>(timestamps.size());
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
        output << "submit outcome=" << result.outcome << " reason=" << result.rejection_reason
               << " disposition=" << result.payload_disposition << '\n';
    }

    void LogControl(const std::string& event) {
        if (event_log_.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::ofstream output(event_log_, std::ios::app);
        output << "control " << event << '\n';
    }

    rodaq::Result<void> ValidatePublishedViews(
        rodaq_stream_handle stream, rodaq_stream_descriptor_handle descriptor,
        std::string_view expected_name) {
        auto stream_view = runtime_.canonical.get_stream(stream);
        if (!stream_view.has_value() || stream_view.value().stream_id == 0) {
            return fail(rodaq::Error(
                rodaq::ErrorCode::InternalError, "fake_acquisition", 0,
                "canonical Stream view is invalid"));
        }
        auto descriptor_view = runtime_.canonical.get_stream_descriptor(descriptor);
        if (!descriptor_view.has_value()) {
            return fail(descriptor_view.error());
        }
        const rodaq_string_view name = descriptor_view.value().name;
        const std::string_view published_name(name.data, name.size);
        if (published_name != expected_name) {
            return fail(rodaq::Error(
                rodaq::ErrorCode::InternalError, "fake_acquisition", 0,
                "published Descriptor view was mutated"));
        }
        auto key_set =
            runtime_.canonical.GetTimestampKeySet(descriptor_view.value().timestamp_key_set_id);
        if (!key_set.has_value() || key_set.value().key_count != timestamp_key_count_ + 1U) {
            return fail(rodaq::Error(
                rodaq::ErrorCode::InternalError, "fake_acquisition", 0,
                "canonical TimestampKeySet has the wrong size"));
        }
        const rodaq_timestamp_key& ingress_key = key_set.value().keys[0];
        const std::string_view ingress_role(ingress_key.role.data, ingress_key.role.size);
        if (ingress_role != "rodaq/acquisition_ingress" ||
            descriptor_view.value().transport_timestamp_key_count != timestamp_key_count_) {
            return fail(rodaq::Error(
                rodaq::ErrorCode::InternalError, "fake_acquisition", 0,
                "canonical/transport timestamp separation is invalid"));
        }
        return {};
    }

    rodaq::Result<void> ValidateOrRecordStaleHandles() {
        if (stale_handle_file_.empty()) {
            return {};
        }
        std::ifstream input(stale_handle_file_);
        if (input) {
            std::uint64_t old_stream = 0;
            std::uint64_t old_descriptor = 0;
            std::uint64_t old_ingress_clock = 0;
            input >> old_stream >> old_descriptor >> old_ingress_clock;
            if (!input) {
                return fail(rodaq::Error(
                    rodaq::ErrorCode::InvalidArgument, "fake_acquisition", 0,
                    "stale handle fixture is invalid"));
            }
            auto stream = runtime_.canonical.get_stream(rodaq_stream_handle{old_stream});
            auto descriptor = runtime_.canonical.get_stream_descriptor(
                rodaq_stream_descriptor_handle{old_descriptor});
            auto ingress = runtime_.canonical.get_clock(rodaq_clock_handle{old_ingress_clock});
            if (stream.has_value() || descriptor.has_value() || ingress.has_value()) {
                return fail(rodaq::Error(
                    rodaq::ErrorCode::InternalError, "fake_acquisition", 0,
                    "stale activation handle resolved"));
            }
            LogControl("stale_handles_rejected");
            return {};
        }

        std::ofstream output(stale_handle_file_);
        if (!output || streams_.empty() || descriptors_.empty()) {
            return fail(rodaq::Error(
                rodaq::ErrorCode::InternalError, "fake_acquisition", 0,
                "failed to record stale handle fixture"));
        }
        auto descriptor = runtime_.canonical.get_stream_descriptor(descriptors_.front());
        if (!descriptor.has_value()) {
            return fail(descriptor.error());
        }
        auto key_set =
            runtime_.canonical.GetTimestampKeySet(descriptor.value().timestamp_key_set_id);
        if (!key_set.has_value() || key_set.value().key_count == 0) {
            return fail(rodaq::Error(
                rodaq::ErrorCode::InternalError, "fake_acquisition", 0,
                "failed to inspect ingress Clock fixture"));
        }
        output << streams_.front().value << ' ' << descriptors_.front().value << ' '
               << key_set.value().keys[0].clock.value << '\n';
        return output ? rodaq::Result<void>()
                      : fail(rodaq::Error(
                            rodaq::ErrorCode::InternalError, "fake_acquisition", 0,
                            "failed to record stale handle fixture"));
    }

    rodaq::Result<void> RegisterProjection(
        rodaq_stream_descriptor_handle descriptor, std::string_view suffix = {}) {
        if (projection_id_.empty()) {
            return {};
        }
        const std::string bytes =
            "projection:" + std::to_string(descriptor.value) + std::string(suffix);
        auto result = runtime_.projection.Register(
            descriptor, projection_id_, reinterpret_cast<const std::uint8_t*>(bytes.data()),
            bytes.size());
        if (!result.has_value()) {
            return result;
        }
        LogControl("projection descriptor=" + std::to_string(descriptor.value));
        return {};
    }

    static rodaq_string_view fixture_string(std::string_view value) noexcept {
        return rodaq_string_view{value.data(), value.size()};
    }

    rodaq::Result<rodaq_stream_descriptor_handle> PublishDescriptor(
        rodaq_stream_handle stream, std::string_view name) {
        std::vector<std::string> roles;
        roles.reserve(timestamp_key_count_);
        for (std::size_t index = 0; index < timestamp_key_count_; ++index) {
            if (timestamp_scenario_ == "ingress_role" && index == 0) {
                roles.emplace_back("rodaq/acquisition_ingress");
            } else if (index == 0) {
                roles.emplace_back("sample_time");
            } else {
                roles.emplace_back("sample_time_" + std::to_string(index));
            }
        }
        std::vector<rodaq_timestamp_key> timestamp_keys;
        timestamp_keys.reserve(roles.size());
        for (const std::string& role : roles) {
            timestamp_keys.push_back(rodaq_timestamp_key{fixture_string(role), clock_});
        }
        return runtime_.acquisition.PublishDescriptor(rodaq_stream_descriptor_registration{
            RODAQ_STREAM_DESCRIPTOR_REGISTRATION_MIN_STRUCT_SIZE,
            RODAQ_TRUE,
            0,
            stream,
            rodaq_string_view{name.data(), name.size()},
            schema_,
            fixture_string("raw"),
            timestamp_keys.data(),
            static_cast<std::uint32_t>(timestamp_keys.size()),
            0,
        });
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
    std::int64_t descriptor_change_after_ = -1;
    std::string binding_scenario_;
    std::string lifecycle_scenario_;
    bool pause_before_switch_ = false;
    std::string stale_handle_file_;
    std::string projection_id_;
    bool projection_before_start_ = false;
    std::int64_t projection_delay_after_ = -1;
    std::string projection_registration_mode_;
    std::string timestamp_scenario_;
    std::size_t timestamp_key_count_ = 1;
    std::string event_log_;
    std::mutex log_mutex_;
    std::mutex switch_mutex_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::size_t> commit_count_{0};
    std::vector<std::thread> producers_;
    std::vector<rodaq_stream_handle> streams_;
    std::vector<rodaq_stream_descriptor_handle> descriptors_;
    std::vector<bool> switched_;
    rodaq_clock_handle clock_{};
    rodaq_schema_handle schema_{};
    std::optional<rodaq_source_handle> source_;
};

}  // namespace

RODAQ_EXPORT_ACQUISITION_PLUGIN(
    FakeAcquisitionPlugin, rodaq::plugin::generated::rodaq_fake_acquisition_plugin::descriptor)
