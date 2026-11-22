#include <rodaq/acquisition/interface.hpp>
#include <rodaq/acquisition/service.hpp>
#include <rodaq/plugin/authoring.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <sys/random.h>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
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
using namespace std::chrono_literals;

struct Stream {
    std::string key;
    rodaq_stream_id_t id{0};
    acquisition::DescriptorToken token{};
    rodaq_source_id_t source{};
};

// Qualification-only protocol source: one explicit worker, one reused borrowed
// payload and no application backlog. Core callback concurrency is never above
// one. Quiesce joins only after actual callbacks end; a timeout retains all
// worker/service/input backing and cannot authorize Foundation destruction.
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
                stream.source.kind = RODAQ_SOURCE_ID_RTPS_GUID;
                stream.source.size = 16;
                // A new synthetic native producer has a new GUID. Reusing a
                // fixed GUID/session=1 across Instance restarts would falsely
                // claim SourceSequence continuity when the counter restarts.
                std::size_t offset = 0;
                while (offset != 16) {
                    const auto count = getrandom(stream.source.data + offset, 16 - offset, 0);
                    if (count < 0 && errno == EINTR) {
                        continue;
                    }
                    if (count <= 0) {
                        return Status(StatusCode::Unavailable, {});
                    }
                    offset += static_cast<std::size_t>(count);
                }
                streams_.push_back(std::move(stream));
            }
            auto service = setup.service<acquisition::Service>();
            if (!service.ok()) {
                return service.status();
            }
            service_.emplace(std::move(service.value()));
            return setup.provide<acquisition::Interface>(*this);
        } catch (const nlohmann::json::exception&) {
            return Status(StatusCode::InvalidArgument, {});
        } catch (const std::invalid_argument&) {
            return Status(StatusCode::InvalidArgument, {});
        }
    }

    Result<void> prepare(const acquisition::PrepareConfig& config, Deadline deadline) {
        if (prepared_ || stopping_.load(std::memory_order_acquire) || mode_ == "fail_prepare") {
            return Status(StatusCode::InvalidState, {});
        }
        if (config.stream_count != streams_.size() || config.max_callback_concurrency == 0) {
            return Status(StatusCode::InvalidArgument, {});
        }
        // Validate the whole bijection/resource envelope before any callback.
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
        for (auto& stream : streams_) {
            auto proposed = propose(stream, false, deadline);
            if (!proposed.ok()) {
                return proposed.status();
            }
            auto ready = report(RODAQ_ACQUISITION_EVENT_STREAM_READY, StatusCode::Ok, stream.id);
            if (!ready.ok()) {
                return ready.status();
            }
        }
        auto transport = report(RODAQ_ACQUISITION_EVENT_TRANSPORT_READY, StatusCode::Ok);
        if (!transport.ok()) {
            return transport.status();
        }
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
        // Publish the wait predicate under the same mutex as wait_for. An
        // atomic store plus notify outside that domain can lose the wakeup
        // between the worker's predicate check and its actual wait.
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
        // Failure injection does not invent quiesce success. Still reclaim a
        // worker whose actual return is known; Framework retains the failed
        // Instance until its own recovery policy permits release.
        return mode_ == "fail_quiesce" ? Result<void>{Status(StatusCode::Unavailable, {})}
                                       : Result<void>{};
    }

    Result<void> shutdown(Deadline deadline) {
        if (mode_ == "late_shutdown") {
            // Framework close is single-flight. Observer expiry neither calls
            // this twice nor proves that the delayed Native owner returned.
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
    Result<void> propose(Stream& stream, bool changed, Deadline deadline) {
        const std::string_view schema = changed ? R"({"version":2})" : R"({"version":1})";
        acquisition::DescriptorProposal proposal{};
        proposal.header.struct_size = sizeof(proposal);
        proposal.stream_id = stream.id;
        proposal.data_profile = RODAQ_DATA_PROFILE_GENERIC;
        proposal.channel_name = {stream.key.data(), static_cast<std::uint32_t>(stream.key.size())};
        proposal.message_encoding = {"raw", 3};
        proposal.schema_name = {"fake.payload", 12};
        proposal.schema_encoding = {"jsonschema", 10};
        proposal.schema = {
            reinterpret_cast<const std::uint8_t*>(schema.data()),
            static_cast<std::uint32_t>(schema.size())};
        auto proposed = service_->propose_descriptor(proposal, deadline);
        if (!proposed.ok()) {
            return proposed.status();
        }
        stream.token = proposed.value();
        return {};
    }

    Result<void> report(
        rodaq_acquisition_event_kind_t kind, StatusCode code, rodaq_stream_id_t stream = 0) {
        acquisition::AcquisitionEvent event{};
        event.header.struct_size = sizeof(event);
        event.kind = kind;
        event.status = static_cast<rodaq_status_code_t>(code);
        event.stream_id = stream;
        event.has_stream_id = stream != 0 ? 1 : 0;
        return service_->report_acquisition_event(event);
    }

    Result<void> source_event(const Stream& stream, rodaq_source_event_kind_t kind) {
        acquisition::SourceEvent event{};
        event.header.struct_size = sizeof(event);
        event.stream_id = stream.id;
        event.source_id = stream.source;
        event.has_source_id = 1;
        event.kind = kind;
        if (kind == RODAQ_SOURCE_EVENT_SESSION_STARTED ||
            kind == RODAQ_SOURCE_EVENT_SESSION_ENDED) {
            event.source_session_id = 1;
            event.has_source_session_id = 1;
        }
        return service_->report_source_event(event);
    }

    Result<void> emit(Stream& stream, std::uint64_t sequence) {
        for (std::size_t i = 0; i < payload_.size(); ++i) {
            payload_[i] = static_cast<std::uint8_t>(sequence + i);
        }
        acquisition::RecordAttempt attempt{};
        attempt.header.struct_size = sizeof(attempt);
        attempt.stream_id = stream.id;
        attempt.descriptor_token = stream.token;
        attempt.source_id = stream.source;
        attempt.source_session_id = 1;
        attempt.source_sequence = sequence;
        attempt.sequence_scope = RODAQ_SEQUENCE_SCOPE_SOURCE_SESSION;
        attempt.has_source_id = 1;
        attempt.has_source_session_id = 1;
        attempt.has_source_sequence = 1;
        attempt.payload = {{sizeof(rodaq_payload_t)}, payload_.data(), payload_.size()};
        if (mode_ == "invalid_attempt") {
            attempt.reserved[0] = 1;
        }
        if (mode_ == "stale_token") {
            // Fault injection only: normal code never invents a Core token.
            ++attempt.descriptor_token.generation;
        }
        const auto submitted = service_->submit_record(attempt);
        std::fill(
            payload_.begin(), payload_.end(), 0xee);  // Reuse immediately after actual return.
        return submitted.ok() ? Result<void>{} : Result<void>{submitted.status()};
    }

    Result<void> produce() {
        for (const auto& stream : streams_) {
            auto discovered = source_event(stream, RODAQ_SOURCE_EVENT_DISCOVERED);
            if (!discovered.ok()) {
                return discovered.status();
            }
            auto session = source_event(stream, RODAQ_SOURCE_EVENT_SESSION_STARTED);
            if (!session.ok()) {
                return session.status();
            }
        }
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
                    auto deadline = Deadline::after(2s);
                    if (!deadline.ok()) {
                        return deadline.status();
                    }
                    auto proposed = propose(stream, true, deadline.value());
                    if (!proposed.ok()) {
                        return proposed.status();
                    }
                }
                if (stopping_.load(std::memory_order_acquire)) {
                    break;
                }
                auto emitted = emit(stream, sequence);
                if (!emitted.ok()) {
                    return emitted.status();
                }
            }
            if (sequence == std::numeric_limits<std::uint64_t>::max()) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait_for(lock, std::chrono::microseconds(period_us_), [&] {
                return stopping_.load(std::memory_order_acquire);
            });
        }
        for (const auto& stream : streams_) {
            auto ended = source_event(stream, RODAQ_SOURCE_EVENT_SESSION_ENDED);
            if (!ended.ok()) {
                return ended.status();
            }
        }
        return {};
    }

    void run() noexcept {
        try {
            auto produced = produce();
            if (!produced.ok()) {
                (void)report(RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE, produced.status().code());
            }
        } catch (...) {
            // No third-party/Service exception can escape a protocol worker.
            try {
                (void)report(RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE, StatusCode::Internal);
            } catch (...) {
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        changed_.notify_all();
    }

    std::optional<acquisition::ServiceReference> service_;
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
    std::thread worker_;  // Joined by successful quiesce, never detached.
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
