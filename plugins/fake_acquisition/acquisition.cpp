#include <rodaq/acquisition/interface.hpp>
#include <rodaq/plugin/authoring.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace rodaq::fake_acquisition {
namespace {

namespace acquisition = rodaq::acquisition;
using plugin::Deadline;

struct Stream {
    std::string key;
    rodaq_stream_id_t id{0};
    rodaq_descriptor_id_t descriptor_id{0};
};

// Qualification-only protocol source: one explicit worker, one reused borrowed
// payload and no application backlog. Core callback concurrency is never above
// one. Quiesce joins only after actual callbacks end.
class FakeAcquisition {
public:
    Result<void> configure(plugin::PluginSetup& setup) {
        try {
            const auto config = nlohmann::json::parse(setup.config());
            if (!config.is_object() || !config.contains("streams") ||
                !config["streams"].is_array() || config["streams"].empty() ||
                config["streams"].size() > 16 || config.size() > 7) {
                return Status(StatusCode::InvalidArgument, {});
            }
            for (const auto& item : config.items()) {
                if (item.key() != "streams" && item.key() != "mode" &&
                    item.key() != "payload_bytes" && item.key() != "period_us" &&
                    item.key() != "record_limit" && item.key() != "schema_change_after" &&
                    item.key() != "delay_ms") {
                    return Status(StatusCode::InvalidArgument, {});
                }
            }
            mode_ = config.value("mode", std::string("normal"));
            constexpr std::string_view modes[]{
                "normal",       "fail_prepare",     "fail_start",     "late_start",
                "fail_quiesce", "fail_shutdown",    "terminal_event", "invalid_attempt",
                "stale_token",  "oversize_attempt", "late_shutdown",  "silent"};
            if (std::find(std::begin(modes), std::end(modes), mode_) == std::end(modes)) {
                return Status(StatusCode::InvalidArgument, {});
            }
            const auto integer = [&](const char* name, std::uint64_t fallback, std::uint64_t max) {
                if (!config.contains(name)) {
                    return fallback;
                }
                if (!config[name].is_number_unsigned() || config[name].get<std::uint64_t>() > max) {
                    throw std::invalid_argument("bounded unsigned option required");
                }
                return config[name].get<std::uint64_t>();
            };
            const auto payload = integer("payload_bytes", 8, 65536);
            period_us_ = integer("period_us", 1000, 1000000);
            record_limit_ = integer("record_limit", 0, 1000000);
            schema_change_after_ = integer("schema_change_after", 0, 1000000);
            delay_ms_ = integer("delay_ms", 100, 5000);
            if (period_us_ == 0) {
                return Status(StatusCode::InvalidArgument, {});
            }
            payload_.resize(static_cast<std::size_t>(payload));
            streams_.reserve(config["streams"].size());
            for (const auto& key : config["streams"]) {
                if (!key.is_string()) {
                    return Status(StatusCode::InvalidArgument, {});
                }
                auto text = key.get<std::string>();
                if (text.empty() || text.size() > 128 || text.find('\0') != std::string::npos ||
                    !plugin::abi::valid_string(
                        {text.data(), static_cast<std::uint32_t>(text.size())}) ||
                    std::any_of(streams_.begin(), streams_.end(), [&](const auto& stream) {
                        return stream.key == text;
                    })) {
                    return Status(StatusCode::InvalidArgument, {});
                }
                Stream stream;
                stream.key = std::move(text);
                streams_.push_back(std::move(stream));
            }
            return setup.provide<acquisition::Interface>(*this);
        } catch (const nlohmann::json::exception&) {
            return Status(StatusCode::InvalidArgument, {});
        } catch (const std::invalid_argument&) {
            return Status(StatusCode::InvalidArgument, {});
        }
    }

    Result<void> prepare(
        const acquisition::PrepareConfig& config, acquisition::Callbacks callbacks, Deadline) {
        if (prepared_ || stopping_.load(std::memory_order_acquire) || mode_ == "fail_prepare") {
            return Status(StatusCode::InvalidState, {});
        }
        if (!callbacks || config.stream_count != streams_.size() ||
            config.max_callback_concurrency == 0) {
            return Status(StatusCode::InvalidArgument, {});
        }
        for (std::uint32_t i = 0; i < config.stream_count; ++i) {
            const auto& binding = config.streams[i];
            const std::string_view key(binding.stream_key.data, binding.stream_key.size);
            auto stream = std::find_if(streams_.begin(), streams_.end(), [&](const auto& entry) {
                return entry.key == key;
            });
            if (stream == streams_.end() || stream->id != 0 ||
                (mode_ != "oversize_attempt" && payload_.size() > binding.max_record_bytes)) {
                return Status(StatusCode::InvalidArgument, {});
            }
            for (const auto& entry : streams_) {
                if (entry.id == binding.stream_id) {
                    return Status(StatusCode::InvalidArgument, {});
                }
            }
            stream->id = binding.stream_id;
        }
        callbacks_ = callbacks;
        for (auto& stream : streams_) {
            auto bound = bind_descriptor(stream);
            if (!bound.ok()) {
                return bound.status();
            }
            report(RODAQ_ACQUISITION_EVENT_STREAM_READY, StatusCode::Ok, stream.id);
        }
        report(RODAQ_ACQUISITION_EVENT_TRANSPORT_READY, StatusCode::Ok);
        prepared_ = true;
        return {};
    }

    Result<void> start(Deadline) {
        if (mode_ == "late_start") {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepared_ || started_ || stopping_.load(std::memory_order_acquire) ||
            mode_ == "fail_start") {
            return Status(StatusCode::InvalidState, {});
        }
        started_ = true;
        done_ = false;
        try {
            worker_ = std::thread([this] { run(); });
        } catch (const std::system_error&) {
            done_ = true;
            return Status(StatusCode::ResourceExhausted, {});
        }
        return {};
    }

    Result<void> quiesce(Deadline deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        stopping_.store(true, std::memory_order_release);
        changed_.notify_all();
        while (!done_) {
            auto remaining = deadline.remaining();
            if (!remaining.ok()) {
                return remaining.status();
            }
            if (remaining.value() == std::chrono::nanoseconds::zero()) {
                return Status(StatusCode::DeadlineExceeded, {});
            }
            if (deadline.is_infinite()) {
                changed_.wait(lock);
            } else {
                changed_.wait_for(lock, remaining.value());
            }
        }
        lock.unlock();
        if (worker_.joinable()) {
            worker_.join();
        }
        return mode_ == "fail_quiesce" ? Result<void>{Status(StatusCode::Unavailable, {})}
                                       : Result<void>{};
    }

    Result<void> shutdown(Deadline deadline) {
        if (mode_ == "late_shutdown") {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
        auto stopped = quiesce(deadline);
        if (!stopped.ok()) {
            return stopped.status();
        }
        return mode_ == "fail_shutdown" ? Result<void>{Status(StatusCode::Unavailable, {})}
                                        : Result<void>{};
    }

private:
    Result<void> bind_descriptor(Stream& stream) {
        constexpr std::string_view kSchema = R"({"version":1})";
        acquisition::Descriptor descriptor{};
        descriptor.header.struct_size = sizeof(descriptor);
        descriptor.stream_id = stream.id;
        descriptor.channel_name = {
            stream.key.data(), static_cast<std::uint32_t>(stream.key.size())};
        descriptor.message_encoding = {"raw", 3};
        descriptor.schema_name = {"fake.payload", 12};
        descriptor.schema_encoding = {"jsonschema", 10};
        descriptor.schema = {
            reinterpret_cast<const std::uint8_t*>(kSchema.data()),
            static_cast<std::uint32_t>(kSchema.size())};
        auto id = callbacks_.prepare_descriptor(stream.id, descriptor);
        if (!id.ok()) {
            return id.status();
        }
        stream.descriptor_id = id.value();
        return {};
    }

    void report(rodaq_acquisition_event_kind_t kind, StatusCode code, rodaq_stream_id_t stream = 0)
        const noexcept {
        acquisition::AcquisitionEvent event{};
        event.header.struct_size = sizeof(event);
        event.kind = kind;
        event.status = static_cast<rodaq_status_code_t>(code);
        event.stream_id = stream;
        event.has_stream_id = stream != 0 ? 1 : 0;
        callbacks_.report_event(event);
    }

    void emit(Stream& stream, std::uint64_t sequence) {
        for (std::size_t i = 0; i < payload_.size(); ++i) {
            payload_[i] = static_cast<std::uint8_t>(sequence + i);
        }
        acquisition::RecordAttempt attempt{};
        attempt.header.struct_size = sizeof(attempt);
        attempt.stream_id = stream.id;
        attempt.descriptor_id = stream.descriptor_id;
        attempt.payload = {payload_.data(), static_cast<std::uint32_t>(payload_.size())};
        if (mode_ == "invalid_attempt") {
            attempt.reserved[0] = 1;
        }
        if (mode_ == "stale_token") {
            ++attempt.descriptor_id;
        }
        (void)callbacks_.submit_record(attempt);
        std::fill(payload_.begin(), payload_.end(), 0xee);
    }

    Result<void> produce() {
        if (mode_ == "terminal_event") {
            return Status(StatusCode::Unavailable, {});
        }
        if (mode_ == "silent") {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [&] { return stopping_.load(std::memory_order_acquire); });
        }
        for (std::uint64_t sequence = 1; !stopping_.load(std::memory_order_acquire); ++sequence) {
            if (record_limit_ != 0 && sequence > record_limit_) {
                break;
            }
            for (auto& stream : streams_) {
                if (stopping_.load(std::memory_order_acquire)) {
                    break;
                }
                if (schema_change_after_ != 0 && sequence == schema_change_after_ + 1) {
                    // Bind-once contract: incompatible schema change fails closed.
                    report(RODAQ_ACQUISITION_EVENT_STREAM_UNAVAILABLE, StatusCode::Ok, stream.id);
                    return Status(StatusCode::Conflict, "Incompatible runtime schema change");
                }
                if (stopping_.load(std::memory_order_acquire)) {
                    break;
                }
                emit(stream, sequence);
            }
            if (sequence == std::numeric_limits<std::uint64_t>::max()) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait_for(lock, std::chrono::microseconds(period_us_), [&] {
                return stopping_.load(std::memory_order_acquire);
            });
        }
        return {};
    }

    void run() noexcept {
        try {
            auto produced = produce();
            if (!produced.ok()) {
                report(RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE, produced.status().code());
            }
        } catch (...) {
            report(RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE, StatusCode::Internal);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        changed_.notify_all();
    }

    acquisition::Callbacks callbacks_;
    std::vector<Stream> streams_;
    std::vector<std::uint8_t> payload_;
    std::string mode_{"normal"};
    std::uint64_t period_us_{1000};
    std::uint64_t record_limit_{0};
    std::uint64_t schema_change_after_{0};
    std::uint64_t delay_ms_{100};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    bool prepared_{false};
    bool started_{false};
    bool done_{true};
};

class FakeAcquisitionPlugin final : public plugin::PluginBase {
public:
    Result<void> configure(plugin::PluginSetup& setup) override {
        return backend_.configure(setup);
    }

    Result<void> shutdown(Deadline deadline) override { return backend_.shutdown(deadline); }

private:
    FakeAcquisition backend_;
};

}  // namespace
}  // namespace rodaq::fake_acquisition

RODAQ_DEFINE_PLUGIN(rodaq::fake_acquisition::FakeAcquisitionPlugin)
