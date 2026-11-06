#include <rodaq/plugin/acquisition_plugin.hpp>

#include "rodaq_fake_acquisition_plugin_metadata.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
    std::string release_block_file;
};

struct TransitionState {
    rodaq_stream_handle stream{};
    std::mutex mutex;
    std::condition_variable condition;
    std::uint64_t transition = 0;
    bool terminal = false;
    bool committed = false;
    rodaq_binding_abort_disposition disposition = RODAQ_BINDING_ABORT_KEEP_PAUSED;
};

bool file_exists(const std::string& path) noexcept {
    try {
        std::ifstream input(path);
        return input.good();
    } catch (...) {
        return false;
    }
}

void create_marker(const std::string& path) noexcept {
    try {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.put('\0');
    } catch (...) {
    }
}

void release_anchor(void* context) noexcept {
    auto* anchor = static_cast<Anchor*>(context);
    if (!anchor->release_block_file.empty()) {
        const std::string blocked_file = anchor->release_block_file + ".blocked";
        const std::string continue_file = anchor->release_block_file + ".continue";
        create_marker(blocked_file);
        while (!file_exists(continue_file)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    delete anchor;
}

std::string config_string(
    const rodaq::plugin::ConfigView& view, std::string_view key, std::string default_value) {
    auto child = view.find(key);
    if (!child.ok()) {
        return default_value;
    }
    auto value = child.value().as_string();
    return value.ok() ? std::string(value.value()) : std::move(default_value);
}

std::int64_t config_int(
    const rodaq::plugin::ConfigView& view, std::string_view key, std::int64_t default_value) {
    auto child = view.find(key);
    if (!child.ok()) {
        return default_value;
    }
    auto value = child.value().as_int64();
    return value.ok() ? value.value() : default_value;
}

class FakeAcquisitionPlugin final : public rodaq::plugin::AcquisitionPlugin {
public:
    ~FakeAcquisitionPlugin() override {
        log_control("destroy");
        if (!destroy_block_file_.empty()) {
            const std::string blocked_file = destroy_block_file_ + ".blocked";
            const std::string continue_file = destroy_block_file_ + ".continue";
            create_marker(blocked_file);
            while (!file_exists(continue_file)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    rodaq::Result<void> bind(rodaq::plugin::AcquisitionContext runtime) override {
        if (stage_ != Stage::Created) {
            return invalid_state();
        }
        runtime_ = std::move(runtime);
        stage_ = Stage::Bound;
        return {};
    }

    rodaq::Result<void> configure(const rodaq::plugin::ConfigView& config) override {
        if (stage_ != Stage::Bound) {
            return invalid_state();
        }
        source_id_ = config_string(config, "source_id", "fake-source");
        ownership_scenario_ = config_string(config, "ownership_scenario", "");
        ownership_file_ = config_string(config, "ownership_file", "");
        producer_wait_file_ = config_string(config, "producer_wait_file", "");
        plugin_health_file_ = config_string(config, "plugin_health_file", "");
        stream_count_ = static_cast<std::size_t>(config_int(config, "streams", 1));
        records_per_stream_ = static_cast<std::size_t>(config_int(config, "records_per_stream", 1));
        producer_threads_ = static_cast<std::size_t>(config_int(config, "producer_threads", 1));
        payload_size_ = static_cast<std::size_t>(config_int(config, "payload_size", 16));
        payload_mode_ = config_string(config, "payload_mode", "copy");
        producer_stream_mode_ = config_string(config, "producer_stream_mode", "all");
        record_interval_us_ = config_int(config, "record_interval_us", 0);
        descriptor_change_after_ = config_int(config, "descriptor_change_after", -1);
        binding_scenario_ = config_string(config, "binding_scenario", "");
        lifecycle_scenario_ = config_string(config, "lifecycle_scenario", "");
        pause_before_switch_ = config_int(config, "pause_before_switch", 0) != 0;
        stale_handle_file_ = config_string(config, "stale_handle_file", "");
        projection_id_ = config_string(config, "projection_id", "");
        projection_before_start_ = config_int(config, "projection_before_start", 0) != 0;
        projection_delay_after_ = config_int(config, "projection_delay_after", -1);
        projection_registration_mode_ = config_string(config, "projection_registration_mode", "");
        plugin_local_lock_ = config_int(config, "plugin_local_lock", 0) != 0;
        timestamp_scenario_ = config_string(config, "timestamp_scenario", "");
        timestamp_key_count_ =
            static_cast<std::size_t>(config_int(config, "timestamp_key_count", 1));
        plugin_health_after_ = config_int(config, "plugin_health_after", -1);
        plugin_health_scenario_ = config_string(config, "plugin_health_scenario", "");
        payload_release_block_file_ = config_string(config, "payload_release_block_file", "");
        destroy_block_file_ = config_string(config, "destroy_block_file", "");
        event_log_ = config_string(config, "event_log", "");
        log_control(
            "configure streams=" + std::to_string(stream_count_) +
            " records=" + std::to_string(records_per_stream_) +
            " producers=" + std::to_string(producer_threads_) + " payload_mode=" + payload_mode_);
        if (stream_count_ == 0 || records_per_stream_ == 0 || producer_threads_ == 0 ||
            timestamp_key_count_ == 0 ||
            (producer_stream_mode_ != "all" && producer_stream_mode_ != "exclusive") ||
            (producer_stream_mode_ == "exclusive" && producer_threads_ != stream_count_) ||
            record_interval_us_ < 0 ||
            (payload_mode_ != "copy" && payload_mode_ != "adopt" && payload_mode_ != "retain")) {
            return invalid_argument();
        }
        runtime_.common.log(RODAQ_LOG_INFO, "plugin.fake", "Acquisition configured");
        stage_ = Stage::Configured;
        return {};
    }

    rodaq::Result<void> start() override {
        if (stage_ != Stage::Configured) {
            return invalid_state();
        }
        stage_ = Stage::StartAttempted;
        if (lifecycle_scenario_ == "start_stall") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (lifecycle_scenario_ == "start_failure") {
            return fail(rodaq::Status(
                rodaq::StatusCode::Unavailable, "fake_acquisition", 0,
                "configured acquisition start failure"));
        }
        auto source = runtime_.acquisition.register_source(rodaq_source_registration{
            RODAQ_SOURCE_REGISTRATION_MIN_STRUCT_SIZE,
            0,
            rodaq_bytes_view{
                reinterpret_cast<const std::uint8_t*>(source_id_.data()), source_id_.size()},
        });
        if (!source.ok()) {
            return fail(source.status());
        }
        source_ = source.value();
        auto clock = runtime_.acquisition.register_clock(rodaq_clock_registration{
            RODAQ_CLOCK_REGISTRATION_MIN_STRUCT_SIZE,
            0,
            rodaq_bytes_view{
                reinterpret_cast<const std::uint8_t*>("fake-clock"), sizeof("fake-clock") - 1U},
            1,
            1'000'000'000,
            fixture_string("monotonic"),
        });
        if (!clock.ok()) {
            return fail(clock.status());
        }
        clock_ = clock.value();
        auto schema = runtime_.acquisition.register_schema(rodaq_schema_registration{
            RODAQ_SCHEMA_REGISTRATION_MIN_STRUCT_SIZE,
            RODAQ_FALSE,
            0,
            fixture_string("raw"),
            {},
            rodaq_bytes_view{
                reinterpret_cast<const std::uint8_t*>("schema"), sizeof("schema") - 1U},
        });
        if (!schema.ok()) {
            return fail(schema.status());
        }
        schema_ = schema.value();

        streams_.clear();
        descriptors_.clear();
        switched_.assign(stream_count_, false);
        streams_.reserve(stream_count_);
        descriptors_.reserve(stream_count_);
        transitions_.clear();
        transitions_.reserve(stream_count_);
        for (std::size_t index = 0; index < stream_count_; ++index) {
            const std::string native = "stream-" + std::to_string(index);
            auto stream = runtime_.acquisition.register_stream(rodaq_stream_registration{
                RODAQ_STREAM_REGISTRATION_MIN_STRUCT_SIZE,
                0,
                rodaq_bytes_view{
                    reinterpret_cast<const std::uint8_t*>(native.data()), native.size()},
            });
            if (!stream.ok()) {
                return fail(stream.status());
            }
            auto transition = std::make_unique<TransitionState>();
            transition->stream = stream.value();
            transitions_.push_back(std::move(transition));
            streams_.push_back(stream.value());
        }
        for (std::size_t index = 0; index < stream_count_; ++index) {
            std::string descriptor_name = "fake/0";
            auto descriptor = publish_descriptor(streams_[index], descriptor_name);
            if (!descriptor.ok()) {
                return fail(descriptor.status());
            }
            if (projection_before_start_) {
                auto projection = register_projection(descriptor.value());
                if (!projection.ok()) {
                    return fail(projection.status());
                }
                if (projection_registration_mode_ == "same") {
                    auto duplicate = register_projection(descriptor.value());
                    if (!duplicate.ok()) {
                        return fail(duplicate.status());
                    }
                } else if (projection_registration_mode_ == "conflict") {
                    auto conflicting = register_projection(descriptor.value(), "-different");
                    if (!conflicting.ok()) {
                        return fail(conflicting.status());
                    }
                }
            }
            descriptor_name.assign("mutated-after-publication");
            auto views = validate_published_views(streams_[index], descriptor.value(), "fake/0");
            if (!views.ok()) {
                return fail(views.status());
            }
            descriptors_.push_back(descriptor.value());
        }
        auto ownership = validate_ownership();
        if (!ownership.ok()) {
            return fail(ownership.status());
        }
        auto stale_handles = validate_or_record_stale_handles();
        if (!stale_handles.ok()) {
            return fail(stale_handles.status());
        }

        stopping_.store(false);
        for (std::size_t producer = 0; producer < producer_threads_; ++producer) {
            producers_.emplace_back([this, producer]() { produce(producer); });
        }
        if (lifecycle_scenario_ == "start_partial_failure" ||
            lifecycle_scenario_ == "start_partial_cleanup_failure") {
            return fail(rodaq::Status(
                rodaq::StatusCode::Unavailable, "fake_acquisition", 0,
                "configured partial start failure"));
        }
        stage_ = Stage::Started;
        return {};
    }

    rodaq::Result<void> stop() override {
        log_control("stop-enter");
        if (stage_ == Stage::Stopped) {
            return {};
        }
        if (stage_ != Stage::Started) {
            if (stage_ != Stage::StartAttempted) {
                return invalid_state();
            }
        }
        if (lifecycle_scenario_ == "stop_stall") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        quiesce();
        stage_ = Stage::Stopped;
        log_control("stop");
        if (lifecycle_scenario_ == "stop_failure") {
            return fail(rodaq::Status(
                rodaq::StatusCode::Unavailable, "fake_acquisition", 0, "configured stop failure"));
        }
        return {};
    }

    rodaq::Result<void> prepare_binding_switch(
        rodaq_binding_transition transition, rodaq_stream_handle stream,
        rodaq_stream_descriptor_handle old_descriptor,
        rodaq_stream_descriptor_handle new_descriptor) override {
        std::unique_lock<std::mutex> plugin_lock(plugin_local_mutex_, std::defer_lock);
        if (plugin_local_lock_) {
            plugin_lock.lock();
        }
        log_control(
            "prepare transition=" + std::to_string(transition.value) + " old=" +
            std::to_string(old_descriptor.value) + " new=" + std::to_string(new_descriptor.value));
        TransitionState* state = find_transition_by_stream(stream);
        if (state == nullptr) {
            return invalid_state();
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->transition = transition.value;
            state->terminal = false;
            state->committed = false;
            state->disposition = RODAQ_BINDING_ABORT_KEEP_PAUSED;
        }
        if ((binding_scenario_ == "prepare_failure_initial" && old_descriptor.value == 0) ||
            ((binding_scenario_ == "prepare_failure_switch" ||
              binding_scenario_ == "prepare_failure_abort_stall") &&
             old_descriptor.value != 0)) {
            return fail(rodaq::Status(
                rodaq::StatusCode::Unavailable, "fake_acquisition", 0,
                "configured binding prepare failure"));
        }
        if (binding_scenario_ == "prepare_stall_switch" && old_descriptor.value != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return {};
    }

    rodaq::Result<void> commit_binding_switch(rodaq_binding_transition transition) override {
        log_control("commit transition=" + std::to_string(transition.value));
        TransitionState* state = find_transition(transition.value);
        const std::size_t commit_index = commit_count_.fetch_add(1);
        if (binding_scenario_ == "commit_failure_switch" && commit_index != 0) {
            complete_transition(state, transition.value, false, RODAQ_BINDING_ABORT_KEEP_PAUSED);
            return fail(rodaq::Status(
                rodaq::StatusCode::Unavailable, "fake_acquisition", 0,
                "configured binding commit failure"));
        }
        if (binding_scenario_ == "commit_stall_switch" && commit_index != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        complete_transition(state, transition.value, true, RODAQ_BINDING_ABORT_KEEP_PAUSED);
        return {};
    }

    rodaq::Result<void> abort_binding_switch(
        rodaq_binding_transition transition, rodaq_binding_abort_disposition disposition) override {
        log_control(
            "abort transition=" + std::to_string(transition.value) +
            " disposition=" + std::to_string(disposition));
        if (binding_scenario_ == "prepare_failure_abort_stall") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        complete_transition(
            find_transition(transition.value), transition.value, false, disposition);
        return {};
    }

    rodaq::Result<void> quiesce_stream(rodaq_stream_handle) override { return {}; }

    rodaq::Result<void> quiesce_plugin() override {
        log_control("quiesce");
        if (lifecycle_scenario_ == "quiesce_failure" ||
            lifecycle_scenario_ == "start_partial_cleanup_failure") {
            // Inject an operational barrier error after joining the fixture's
            // native workers. DAQ still cannot assume quiescence on error
            // and must quarantine; the fixture owns its completed threads.
            quiesce();
            return fail(rodaq::Status(
                rodaq::StatusCode::Unavailable, "fake_acquisition", 0,
                "configured quiesce failure"));
        }
        if (lifecycle_scenario_ == "quiesce_stall") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        quiesce();
        return {};
    }

private:
    enum class Stage {
        Created,
        Bound,
        Configured,
        StartAttempted,
        Started,
        Stopped,
    };

    void quiesce() {
        stopping_.store(true);
        for (const auto& transition : transitions_) {
            transition->condition.notify_all();
        }
        for (std::thread& producer : producers_) {
            if (producer.joinable()) {
                producer.join();
            }
        }
        producers_.clear();
    }

    TransitionState* find_transition_by_stream(rodaq_stream_handle stream) {
        const auto found = std::find_if(
            transitions_.begin(), transitions_.end(),
            [stream](const std::unique_ptr<TransitionState>& state) {
                return state->stream.value == stream.value;
            });
        return found == transitions_.end() ? nullptr : found->get();
    }

    TransitionState* find_transition(std::uint64_t transition) {
        const auto found = std::find_if(
            transitions_.begin(), transitions_.end(),
            [transition](const std::unique_ptr<TransitionState>& state) {
                std::lock_guard<std::mutex> lock(state->mutex);
                return state->transition == transition;
            });
        return found == transitions_.end() ? nullptr : found->get();
    }

    void complete_transition(
        TransitionState* state, std::uint64_t transition, bool committed,
        rodaq_binding_abort_disposition disposition) {
        if (state == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->transition != transition) {
                return;
            }
            state->terminal = true;
            state->committed = committed;
            state->disposition = disposition;
        }
        state->condition.notify_all();
    }

    void produce(std::size_t producer_index) {
        while (!stopping_.load()) {
            const auto lifecycle = runtime_.common.lifecycle_snapshot();
            if (lifecycle.ok() && lifecycle.value().normal_acquisition_allowed == RODAQ_TRUE) {
                break;
            }
            if (lifecycle.ok() && lifecycle.value().state == RODAQ_RUNTIME_FAILED) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (!producer_wait_file_.empty()) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!stopping_.load() && !file_exists(producer_wait_file_) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!file_exists(producer_wait_file_)) {
                log_control("producer_wait_failed");
                return;
            }
        }

        const std::size_t first_stream = producer_stream_mode_ == "exclusive" ? producer_index : 0;
        const std::size_t stream_limit = producer_stream_mode_ == "exclusive"
                                             ? std::min(first_stream + 1U, streams_.size())
                                             : streams_.size();
        std::vector<std::uint8_t> source(payload_size_);
        std::vector<rodaq_timestamp_value> timestamps(timestamp_key_count_);
        for (std::size_t stream_index = first_stream;
             stream_index < stream_limit && !stopping_.load(); ++stream_index) {
            auto next_record_at = std::chrono::steady_clock::now();
            for (std::size_t record_index = 0;
                 record_index < records_per_stream_ && !stopping_.load(); ++record_index) {
                if (record_interval_us_ > 0 && record_index != 0) {
                    next_record_at += std::chrono::microseconds(record_interval_us_);
                    std::this_thread::sleep_until(next_record_at);
                }
                if (!submit_one(producer_index, stream_index, record_index, source, timestamps)) {
                    break;
                }
            }
        }
    }

    bool submit_one(
        std::size_t producer_index, std::size_t stream_index, std::size_t record_index,
        std::vector<std::uint8_t>& source, std::vector<rodaq_timestamp_value>& timestamps) {
        if (projection_delay_after_ >= 0 &&
            record_index == static_cast<std::size_t>(projection_delay_after_)) {
            rodaq_stream_descriptor_handle current{};
            {
                std::lock_guard<std::mutex> lock(switch_mutex_);
                current = descriptors_[stream_index];
            }
            auto& transition = *transitions_[stream_index];
            {
                std::lock_guard<std::mutex> transition_lock(transition.mutex);
                transition.terminal = false;
                transition.committed = false;
            }
            rodaq::Result<void> projection;
            {
                std::unique_lock<std::mutex> plugin_lock(plugin_local_mutex_, std::defer_lock);
                if (plugin_local_lock_) {
                    plugin_lock.lock();
                }
                projection = register_projection(current);
            }
            if (!projection.ok()) {
                return false;
            }
            std::unique_lock<std::mutex> transition_lock(transition.mutex);
            transition.condition.wait(transition_lock, [this, &transition] {
                return transition.terminal || stopping_.load();
            });
            if (!transition.terminal || !transition.committed) {
                return false;
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
                    const auto paused = runtime_.acquisition.report_stream(
                        streams_[stream_index], RODAQ_AVAILABILITY_UNAVAILABLE, nullptr);
                    if (!paused.ok()) {
                        return false;
                    }
                }
                {
                    std::lock_guard<std::mutex> transition_lock(transitions_[stream_index]->mutex);
                    transitions_[stream_index]->terminal = false;
                    transitions_[stream_index]->committed = false;
                }
                auto switched_descriptor =
                    publish_descriptor(streams_[stream_index], "fake/switched");
                if (!switched_descriptor.ok()) {
                    return false;
                }
                TransitionState& transition = *transitions_[stream_index];
                if (projection_before_start_) {
                    auto projection = register_projection(switched_descriptor.value());
                    if (!projection.ok()) {
                        return false;
                    }
                }
                std::unique_lock<std::mutex> transition_lock(transition.mutex);
                transition.condition.wait(transition_lock, [this, &transition] {
                    return transition.terminal || stopping_.load();
                });
                if (!transition.terminal) {
                    return false;
                }
                if (transition.committed) {
                    descriptors_[stream_index] = switched_descriptor.value();
                } else if (transition.disposition != RODAQ_BINDING_ABORT_RESUME_OLD) {
                    descriptors_[stream_index] = switched_descriptor.value();
                }
            }
            descriptor = descriptors_[stream_index];
        }
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
            anchor = new Anchor{source, payload_release_block_file_};
            data = anchor->bytes.data();
            owner_context = anchor;
            release = &release_anchor;
            mode = payload_mode_ == "adopt" ? RODAQ_PAYLOAD_ADOPT : RODAQ_PAYLOAD_RETAIN;
        }

        for (std::size_t slot = 0; slot < timestamps.size(); ++slot) {
            std::int64_t reading = static_cast<std::int64_t>(record_index + slot + 1U);
            if (timestamp_scenario_ == "monotonic") {
                const auto monotonic = runtime_.common.monotonic_time_ns();
                if (monotonic.ok()) {
                    reading = static_cast<std::int64_t>(monotonic.value());
                }
            }
            timestamps[slot] = rodaq_timestamp_value{RODAQ_TRUE, 0, reading};
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

        if (ownership_scenario_ == "probe") {
            submission.source = foreign_source_;
            auto rejected = runtime_.acquisition.submit(submission);
            const auto expected_disposition = mode == RODAQ_PAYLOAD_COPY
                                                  ? RODAQ_PAYLOAD_DISPOSITION_NOT_APPLICABLE
                                                  : RODAQ_PAYLOAD_DISPOSITION_CALLER_RESPONSIBLE;
            if (!rejected.ok() || rejected.value().outcome != RODAQ_ADMISSION_PRE_REJECTED ||
                rejected.value().rejection_reason != RODAQ_REJECTION_STALE_HANDLE ||
                rejected.value().payload_disposition != expected_disposition) {
                if (anchor != nullptr &&
                    (!rejected.ok() || rejected.value().payload_disposition ==
                                           RODAQ_PAYLOAD_DISPOSITION_CALLER_RESPONSIBLE)) {
                    delete anchor;
                }
                log_control("ownership_submit_failed");
                return false;
            }
            log_control("ownership_submit_rejected");
            submission.source = *source_;
        }
        auto result = runtime_.acquisition.submit(submission);
        if (!result.ok()) {
            if (anchor != nullptr) {
                delete anchor;
            }
            log_submit_error(result.status());
            return !stopping_.load();
        }
        if (anchor != nullptr &&
            result.value().payload_disposition == RODAQ_PAYLOAD_DISPOSITION_CALLER_RESPONSIBLE) {
            delete anchor;
        }
        log_result(submission, result.value());
        if (plugin_health_scenario_ == "source_recovered" && record_index == 1) {
            if (result.value().rejection_reason != RODAQ_REJECTION_SOURCE_UNAVAILABLE ||
                !runtime_.acquisition.report_source(*source_, RODAQ_AVAILABILITY_AVAILABLE, nullptr)
                     .ok()) {
                log_control("source_recovery_failed");
                return false;
            }
            log_control("source_explicitly_recovered");
        }
        if (plugin_health_after_ >= 0 &&
            record_index == static_cast<std::size_t>(plugin_health_after_)) {
            if (!trigger_plugin_health(stream_index)) {
                return false;
            }
        }
        return !stopping_.load();
    }

    bool trigger_plugin_health(std::size_t stream_index) {
        rodaq_status diagnostic{};
        rodaq::assign_abi_status(
            RODAQ_STATUS_UNAVAILABLE, "fake_acquisition", 0, "configured Plugin health failure",
            &diagnostic);
        const rodaq_acquisition_plugin_state state =
            plugin_health_scenario_ == "terminal" ? RODAQ_ACQUISITION_PLUGIN_TERMINAL_FAILURE
                                                  : RODAQ_ACQUISITION_PLUGIN_RECOVERABLE_FAILURE;
        if (!runtime_.acquisition.report_plugin_state(state, &diagnostic).ok()) {
            return false;
        }
        log_control("plugin_health state=" + plugin_health_scenario_);
        if (plugin_health_scenario_ != "recovered" &&
            plugin_health_scenario_ != "source_recovered") {
            return true;
        }
        if (!runtime_.acquisition.report_plugin_state(RODAQ_ACQUISITION_PLUGIN_AVAILABLE, nullptr)
                 .ok()) {
            return false;
        }
        if (plugin_health_scenario_ == "recovered" &&
            !runtime_.acquisition.report_source(*source_, RODAQ_AVAILABILITY_AVAILABLE, nullptr)
                 .ok()) {
            return false;
        }
        TransitionState& transition = *transitions_[stream_index];
        {
            std::lock_guard<std::mutex> transition_lock(transition.mutex);
            transition.terminal = false;
            transition.committed = false;
        }
        auto descriptor = publish_descriptor(streams_[stream_index], "fake/recovered");
        if (!descriptor.ok()) {
            return false;
        }
        if (projection_before_start_ || !projection_id_.empty()) {
            auto projection = register_projection(descriptor.value());
            if (!projection.ok()) {
                return false;
            }
        }
        std::unique_lock<std::mutex> transition_lock(transition.mutex);
        transition.condition.wait(transition_lock, [this, &transition] {
            return transition.terminal || stopping_.load();
        });
        if (!transition.terminal || !transition.committed) {
            return false;
        }
        std::lock_guard<std::mutex> lock(switch_mutex_);
        descriptors_[stream_index] = descriptor.value();
        if (!plugin_health_file_.empty()) {
            create_marker(plugin_health_file_);
        }
        return true;
    }

    void log_result(const rodaq_record_submission&, const rodaq_acquisition_submit_result& result) {
        if (event_log_.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::ofstream output(event_log_, std::ios::app);
        output << "submit outcome=" << result.outcome << " reason=" << result.rejection_reason
               << " disposition=" << result.payload_disposition << '\n';
    }

    void log_submit_error(const rodaq::Status& error) {
        if (event_log_.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::ofstream output(event_log_, std::ios::app);
        output << "submit error=" << static_cast<std::uint32_t>(error.status_code())
               << " message=" << error.message() << '\n';
    }

    void log_control(const std::string& event) {
        if (event_log_.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::ofstream output(event_log_, std::ios::app);
        output << "control " << event << '\n';
    }

    rodaq::Result<void> validate_published_views(
        rodaq_stream_handle stream, rodaq_stream_descriptor_handle descriptor,
        std::string_view expected_name) {
        auto stream_view = runtime_.canonical.get_stream(stream);
        if (!stream_view.ok() || stream_view.value().stream_id == 0) {
            return fail(rodaq::Status(
                rodaq::StatusCode::InternalError, "fake_acquisition", 0,
                "canonical Stream view is invalid"));
        }
        auto descriptor_view = runtime_.canonical.get_stream_descriptor(descriptor);
        if (!descriptor_view.ok()) {
            return fail(descriptor_view.status());
        }
        const rodaq_string_view name = descriptor_view.value().name;
        const std::string_view published_name(name.data, name.size);
        if (published_name != expected_name) {
            return fail(rodaq::Status(
                rodaq::StatusCode::InternalError, "fake_acquisition", 0,
                "published Descriptor view was mutated"));
        }
        auto key_set =
            runtime_.canonical.get_timestamp_key_set(descriptor_view.value().timestamp_key_set_id);
        if (!key_set.ok() || key_set.value().key_count != timestamp_key_count_ + 1U) {
            return fail(rodaq::Status(
                rodaq::StatusCode::InternalError, "fake_acquisition", 0,
                "canonical TimestampKeySet has the wrong size"));
        }
        const rodaq_timestamp_key& ingress_key = key_set.value().keys[0];
        const std::string_view ingress_role(ingress_key.role.data, ingress_key.role.size);
        if (ingress_role != "rodaq/acquisition_ingress" ||
            descriptor_view.value().transport_timestamp_key_count != timestamp_key_count_) {
            return fail(rodaq::Status(
                rodaq::StatusCode::InternalError, "fake_acquisition", 0,
                "canonical/transport timestamp separation is invalid"));
        }
        return {};
    }

    rodaq::Result<void> validate_ownership() {
        if (ownership_scenario_.empty()) {
            return {};
        }
        auto duplicate = runtime_.acquisition.register_source(rodaq_source_registration{
            RODAQ_SOURCE_REGISTRATION_MIN_STRUCT_SIZE, 0,
            rodaq_bytes_view{
                reinterpret_cast<const std::uint8_t*>(source_id_.data()), source_id_.size()}});
        if (!duplicate.ok() || duplicate.value().value != source_->value) {
            return invalid_state();
        }
        if (ownership_scenario_ == "owner") {
            auto descriptor = runtime_.canonical.get_stream_descriptor(descriptors_.front());
            if (!descriptor.ok()) {
                return fail(descriptor.status());
            }
            auto keys =
                runtime_.canonical.get_timestamp_key_set(descriptor.value().timestamp_key_set_id);
            if (!keys.ok() || keys.value().key_count == 0) {
                return invalid_state();
            }
            std::ofstream output(ownership_file_);
            output << source_->value << ' ' << streams_.front().value << ' ' << clock_.value << ' '
                   << keys.value().keys[0].clock.value << ' ' << source_id_ << '\n';
            if (!output) {
                return invalid_state();
            }
            return {};
        }
        std::ifstream input(ownership_file_);
        rodaq_stream_handle foreign_stream{};
        rodaq_clock_handle foreign_clock{}, ingress_clock{};
        std::string foreign_id;
        input >> foreign_source_.value >> foreign_stream.value >> foreign_clock.value >>
            ingress_clock.value >> foreign_id;
        if (!input) {
            return invalid_state();
        }
        auto conflict = runtime_.acquisition.register_source(rodaq_source_registration{
            RODAQ_SOURCE_REGISTRATION_MIN_STRUCT_SIZE, 0,
            rodaq_bytes_view{
                reinterpret_cast<const std::uint8_t*>(foreign_id.data()), foreign_id.size()}});
        if (conflict.ok() || conflict.status().status_code() != rodaq::StatusCode::Conflict) {
            return invalid_state();
        }
        for (auto availability : {RODAQ_AVAILABILITY_UNAVAILABLE, RODAQ_AVAILABILITY_AVAILABLE}) {
            auto source =
                runtime_.acquisition.report_source(foreign_source_, availability, nullptr);
            auto stream = runtime_.acquisition.report_stream(foreign_stream, availability, nullptr);
            if (source.ok() || source.status().status_code() != rodaq::StatusCode::InvalidState ||
                stream.ok() || stream.status().status_code() != rodaq::StatusCode::InvalidState) {
                return invalid_state();
            }
        }
        auto unknown_source = runtime_.acquisition.report_source(
            rodaq_source_handle{UINT64_MAX}, RODAQ_AVAILABILITY_UNAVAILABLE, nullptr);
        auto unknown_stream = runtime_.acquisition.report_stream(
            rodaq_stream_handle{UINT64_MAX}, RODAQ_AVAILABILITY_UNAVAILABLE, nullptr);
        if (unknown_source.ok() ||
            unknown_source.status().status_code() != rodaq::StatusCode::NotFound ||
            unknown_stream.ok() ||
            unknown_stream.status().status_code() != rodaq::StatusCode::NotFound) {
            return invalid_state();
        }
        const auto own_clock = clock_;
        for (auto clock : {foreign_clock, ingress_clock, rodaq_clock_handle{UINT64_MAX}}) {
            clock_ = clock;
            auto descriptor = publish_descriptor(streams_.front(), "fake/foreign-clock");
            const auto expected = clock.value == UINT64_MAX ? rodaq::StatusCode::NotFound
                                                            : rodaq::StatusCode::InvalidState;
            if (descriptor.ok() || descriptor.status().status_code() != expected) {
                clock_ = own_clock;
                return invalid_state();
            }
        }
        clock_ = own_clock;
        log_control("ownership_control_rejected");
        return {};
    }

    rodaq::Result<void> validate_or_record_stale_handles() {
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
                return fail(rodaq::Status(
                    rodaq::StatusCode::InvalidArgument, "fake_acquisition", 0,
                    "stale handle fixture is invalid"));
            }
            auto stream = runtime_.canonical.get_stream(rodaq_stream_handle{old_stream});
            auto descriptor = runtime_.canonical.get_stream_descriptor(
                rodaq_stream_descriptor_handle{old_descriptor});
            auto ingress = runtime_.canonical.get_clock(rodaq_clock_handle{old_ingress_clock});
            if (stream.ok() || descriptor.ok() || ingress.ok()) {
                return fail(rodaq::Status(
                    rodaq::StatusCode::InternalError, "fake_acquisition", 0,
                    "stale activation handle resolved"));
            }
            log_control("stale_handles_rejected");
            return {};
        }

        std::ofstream output(stale_handle_file_);
        if (!output || streams_.empty() || descriptors_.empty()) {
            return fail(rodaq::Status(
                rodaq::StatusCode::InternalError, "fake_acquisition", 0,
                "failed to record stale handle fixture"));
        }
        auto descriptor = runtime_.canonical.get_stream_descriptor(descriptors_.front());
        if (!descriptor.ok()) {
            return fail(descriptor.status());
        }
        auto key_set =
            runtime_.canonical.get_timestamp_key_set(descriptor.value().timestamp_key_set_id);
        if (!key_set.ok() || key_set.value().key_count == 0) {
            return fail(rodaq::Status(
                rodaq::StatusCode::InternalError, "fake_acquisition", 0,
                "failed to inspect ingress Clock fixture"));
        }
        output << streams_.front().value << ' ' << descriptors_.front().value << ' '
               << key_set.value().keys[0].clock.value << '\n';
        if (!output) {
            return fail(rodaq::Status(
                rodaq::StatusCode::InternalError, "fake_acquisition", 0,
                "failed to record stale handle fixture"));
        }
        return {};
    }

    rodaq::Result<void> register_projection(
        rodaq_stream_descriptor_handle descriptor, std::string_view suffix = {}) {
        if (projection_id_.empty()) {
            return {};
        }
        const std::string bytes =
            "projection:" + std::to_string(descriptor.value) + std::string(suffix);
        auto result = runtime_.projection.register_entry(
            descriptor, projection_id_, reinterpret_cast<const std::uint8_t*>(bytes.data()),
            bytes.size());
        if (!result.ok()) {
            return result;
        }
        log_control("projection descriptor=" + std::to_string(descriptor.value));
        return {};
    }

    static rodaq_string_view fixture_string(std::string_view value) noexcept {
        return rodaq_string_view{value.data(), value.size()};
    }

    rodaq::Result<rodaq_stream_descriptor_handle> publish_descriptor(
        rodaq_stream_handle stream, std::string_view name) {
        std::vector<std::string> roles;
        roles.reserve(timestamp_key_count_);
        for (std::size_t index = 0; index < timestamp_key_count_; ++index) {
            if (timestamp_scenario_ == "ingress_role" && index == 0) {
                roles.emplace_back("rodaq/acquisition_ingress");
            } else if (timestamp_scenario_ == "duplicate" && index == 1) {
                roles.emplace_back("sample_time");
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
        auto result = runtime_.acquisition.publish_descriptor(rodaq_stream_descriptor_registration{
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
        if (result.ok()) {
            log_control("descriptor_published descriptor=" + std::to_string(result.value().value));
        }
        return result;
    }

    static rodaq::Result<void> invalid_state() {
        return rodaq::Status(
            rodaq::StatusCode::InvalidState, "fake_acquisition", 0, "invalid state");
    }

    static rodaq::Result<void> invalid_argument() {
        return rodaq::Status(
            rodaq::StatusCode::InvalidArgument, "fake_acquisition", 0, "invalid config");
    }

    static rodaq::Result<void> fail(const rodaq::Status& error) { return rodaq::Status(error); }

    rodaq::plugin::AcquisitionContext runtime_;
    Stage stage_ = Stage::Created;
    std::size_t stream_count_ = 1;
    std::size_t records_per_stream_ = 1;
    std::size_t producer_threads_ = 1;
    std::size_t payload_size_ = 16;
    std::string source_id_ = "fake-source";
    std::string ownership_scenario_;
    std::string ownership_file_;
    std::string producer_wait_file_;
    std::string plugin_health_file_;
    rodaq_source_handle foreign_source_{};
    std::string payload_mode_ = "copy";
    std::string producer_stream_mode_ = "all";
    std::int64_t record_interval_us_ = 0;
    std::int64_t descriptor_change_after_ = -1;
    std::string binding_scenario_;
    std::string lifecycle_scenario_;
    bool pause_before_switch_ = false;
    std::string stale_handle_file_;
    std::string projection_id_;
    bool projection_before_start_ = false;
    std::int64_t projection_delay_after_ = -1;
    std::string projection_registration_mode_;
    bool plugin_local_lock_ = false;
    std::string timestamp_scenario_;
    std::size_t timestamp_key_count_ = 1;
    std::int64_t plugin_health_after_ = -1;
    std::string plugin_health_scenario_;
    std::string payload_release_block_file_;
    std::string destroy_block_file_;
    std::string event_log_;
    std::mutex log_mutex_;
    std::mutex switch_mutex_;
    std::mutex plugin_local_mutex_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::size_t> commit_count_{0};
    std::vector<std::thread> producers_;
    std::vector<rodaq_stream_handle> streams_;
    std::vector<rodaq_stream_descriptor_handle> descriptors_;
    std::vector<std::unique_ptr<TransitionState>> transitions_;
    std::vector<bool> switched_;
    rodaq_clock_handle clock_{};
    rodaq_schema_handle schema_{};
    std::optional<rodaq_source_handle> source_;
};

}  // namespace

RODAQ_EXPORT_ACQUISITION_PLUGIN(
    FakeAcquisitionPlugin, rodaq::plugin::generated::rodaq_fake_acquisition_plugin::descriptor)
