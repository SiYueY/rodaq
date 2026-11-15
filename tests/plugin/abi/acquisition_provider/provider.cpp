#include <rodaq/acquisition/interface.hpp>
#include <rodaq/acquisition/service.hpp>
#include <rodaq/plugin/authoring.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <thread>

namespace acq = rodaq::acquisition;
using rodaq::plugin::Deadline;

// Bounded callback ABI fixture, not a native protocol implementation.
class AcquisitionProbe {
public:
    rodaq::Result<void> prepare(const acq::PrepareConfig& config, Deadline deadline) {
        if (started_ || config.stream_count != 1 ||
            std::string_view(
                config.streams[0].stream_key.data, config.streams[0].stream_key.size) != "sample" ||
            config.streams[0].max_record_bytes < 4) {
            return rodaq::Status(
                rodaq::StatusCode::InvalidArgument, "Expected exactly configured sample binding");
        }
        stream_ = config.streams[0].stream_id;
        concurrency_ = std::min<std::uint32_t>(config.max_callback_concurrency, workers_.size());
        acq::DescriptorProposal proposal{};
        proposal.header.struct_size = sizeof(proposal);
        proposal.stream_id = stream_;
        proposal.data_profile = RODAQ_DATA_PROFILE_GENERIC;
        proposal.channel_name = {"sample", 6};
        proposal.message_encoding = {"raw", 3};
        auto proposed = service->propose_descriptor(proposal, deadline);
        if (!proposed.ok()) {
            return proposed.status();
        }
        token_ = proposed.value();
        acq::AcquisitionEvent event{};
        event.header.struct_size = sizeof(event);
        event.kind = RODAQ_ACQUISITION_EVENT_STREAM_READY;
        event.has_stream_id = 1;
        event.stream_id = stream_;
        return service->report_acquisition_event(event);
    }

    rodaq::Result<void> start(Deadline) {
        if (started_ || token_.value == 0) {
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
                record.descriptor_token = token_;
                record.payload = {{sizeof(record.payload)}, payload.data(), payload.size()};
                const auto result = service->submit_record(record);
                (void)result;
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

    std::optional<acq::ServiceReference> service;

private:
    std::array<std::thread, 4> workers_;
    rodaq_stream_id_t stream_{0};
    acq::DescriptorToken token_{};
    std::uint32_t concurrency_{0};
    bool started_{false};
};

class ProbePlugin final : public rodaq::plugin::PluginBase {
public:
    rodaq::Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        auto service = setup.service<acq::Service>();
        if (!service.ok()) {
            return service.status();
        }
        probe_.service = std::move(service).value();
        return setup.provide<acq::Interface>(probe_);
    }

    rodaq::Result<void> shutdown(Deadline deadline) override { return probe_.quiesce(deadline); }

private:
    AcquisitionProbe probe_;
};

RODAQ_DEFINE_PLUGIN(ProbePlugin)
