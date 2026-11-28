#include <rodaq/acquisition/interface.hpp>
#include <rodaq/plugin/authoring.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <thread>

namespace acq = rodaq::acquisition;
using rodaq::plugin::Deadline;

// Bounded callback ABI fixture, not a native protocol implementation.
class AcquisitionProbe {
public:
    rodaq::Result<void> prepare(
        const acq::PrepareConfig& config, acq::Callbacks callbacks, Deadline) {
        if (started_ || config.stream_count != 1 ||
            std::string_view(
                config.streams[0].stream_key.data, config.streams[0].stream_key.size) != "sample" ||
            config.streams[0].max_record_bytes < 4) {
            return rodaq::Status(
                rodaq::StatusCode::InvalidArgument, "Expected exactly configured sample binding");
        }
        callbacks_ = callbacks;
        stream_ = config.streams[0].stream_id;
        concurrency_ = std::min<std::uint32_t>(config.max_callback_concurrency, workers_.size());

        acq::Descriptor descriptor{};
        descriptor.header.struct_size = sizeof(descriptor);
        descriptor.stream_id = stream_;
        descriptor.channel_name = {"sample", 6};
        descriptor.message_encoding = {"raw", 3};
        auto bound = callbacks_.prepare_descriptor(stream_, descriptor);
        if (!bound.ok()) {
            return bound.status();
        }
        descriptor_id_ = bound.value();

        acq::AcquisitionEvent event{};
        event.header.struct_size = sizeof(event);
        event.kind = RODAQ_ACQUISITION_EVENT_STREAM_READY;
        event.has_stream_id = 1;
        event.stream_id = stream_;
        callbacks_.report_event(event);
        return {};
    }

    rodaq::Result<void> start(Deadline) {
        if (started_ || descriptor_id_ == 0) {
            return rodaq::Status(
                rodaq::StatusCode::InvalidState, "Probe not prepared or already started");
        }
        started_ = true;
        for (std::uint32_t i = 0; i < concurrency_; ++i) {
            workers_[i] = std::thread([this, i] {
                std::array<std::uint8_t, 4> payload{1, 2, 3, static_cast<std::uint8_t>(i)};
                acq::RecordAttempt record{};
                record.header.struct_size = sizeof(record);
                record.stream_id = stream_;
                record.descriptor_id = descriptor_id_;
                record.payload = {payload.data(), payload.size()};
                (void)callbacks_.submit_record(record);
                payload.fill(0xff);  // Caller copy must already be independent.
            });
        }
        return {};
    }

    rodaq::Result<void> quiesce(Deadline) {
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        started_ = false;
        return {};
    }

private:
    acq::Callbacks callbacks_;
    std::array<std::thread, 4> workers_;
    rodaq_stream_id_t stream_{0};
    rodaq_descriptor_id_t descriptor_id_{0};
    std::uint32_t concurrency_{0};
    bool started_{false};
};

class ProbePlugin final : public rodaq::plugin::PluginBase {
public:
    rodaq::Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        return setup.provide<acq::Interface>(probe_);
    }

    rodaq::Result<void> shutdown(Deadline deadline) override { return probe_.quiesce(deadline); }

private:
    AcquisitionProbe probe_;
};

RODAQ_DEFINE_PLUGIN(ProbePlugin)
