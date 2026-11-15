#include "counter.hpp"

#include <rodaq/acquisition/interface.hpp>
#include <rodaq/acquisition/service.hpp>
#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/registry.hpp>
#include <rodaq/storage/interface.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

namespace plugin = rodaq::plugin;
namespace acquisition = rodaq::acquisition;
namespace storage = rodaq::storage;
using namespace std::chrono_literals;

struct Host {
    rodaq::Result<acquisition::SubmitResult> submit_record(
        const acquisition::RecordAttempt& record) {
        assert(record.payload.logical_size == copied.size());
        std::copy_n(record.payload.data, copied.size(), copied.data());
        ++submissions;
        return acquisition::SubmitResult{
            {sizeof(acquisition::SubmitResult)}, RODAQ_SUBMIT_ACCEPTED, 0};
    }

    rodaq::Result<acquisition::DescriptorToken> propose_descriptor(
        const acquisition::DescriptorProposal& proposal, plugin::Deadline) {
        assert(proposal.stream_id == 42);
        return acquisition::DescriptorToken{21, 31};
    }

    rodaq::Result<void> report_source_event(const acquisition::SourceEvent&) { return {}; }

    rodaq::Result<void> report_acquisition_event(const acquisition::AcquisitionEvent& event) {
        assert(event.stream_id == 42 && event.kind == RODAQ_ACQUISITION_EVENT_STREAM_READY);
        ready = true;
        return {};
    }

    std::array<std::uint8_t, 3> copied{};
    std::uint32_t submissions{0};
    bool ready{false};
};

plugin::Deadline soon() { return plugin::Deadline::after(2s).value(); }

int main(int argc, char** argv) {
    assert(argc == 3);
    const std::string_view family = argv[2];
    assert(family == "counter" || family == "acquisition" || family == "storage");
    plugin::PluginManagerOptions policy;
    policy.search_paths = {argv[1]};
    policy.max_loaded_plugins = 1;
    policy.max_instances = 1;
    policy.max_config_size = 1024;
    policy.shutdown_timeout = 1s;
    auto created = plugin::PluginManager::create(std::move(policy));
    assert(created.ok());
    auto manager = std::move(created).value();
    const auto id = family == "counter"       ? "com.example.counter"
                    : family == "acquisition" ? "com.example.acquisition.compat"
                                              : "com.example.storage.compat";
    auto loaded = manager.load(id, "1.0.0");
    assert(loaded.ok());
    auto library = std::move(loaded).value();
    plugin::ServiceRegistry services;
    auto host = std::make_shared<Host>();
    if (family == "acquisition") {
        assert(services.add_shared<acquisition::Service>(host).ok());
    }
    assert(services.freeze().ok());
    auto initialized = library.create_instance(services, {});
    assert(initialized.ok());
    auto instance = std::move(initialized).value();
    if (family == "counter") {
        auto queried = instance.query<CounterInterface>();
        assert(queried.ok());
        auto counter = std::move(queried).value();
        assert(counter.increment(7).value() == 7 && counter.current().value() == 7);
    } else if (family == "acquisition") {
        auto queried = instance.query<acquisition::Interface>();
        assert(queried.ok());
        auto handle = std::move(queried).value();
        const acquisition::StreamBinding stream{{sizeof(stream)}, 42, {"sample", 6}, 8};
        const acquisition::PrepareConfig config{{sizeof(config)}, &stream, 1, 1};
        assert(handle.prepare(config, soon()).ok());
        assert(handle.start(soon()).ok());
        assert(handle.quiesce(soon()).ok());
        assert(
            host->ready && host->submissions == 1 && host->copied[0] == 3 && host->copied[1] == 2 &&
            host->copied[2] == 1);
    } else {
        auto queried = instance.query<storage::Interface>();
        assert(queried.ok());
        auto handle = std::move(queried).value();
        const storage::PrepareConfig config{
            {sizeof(config)}, 8, 16, 2, 2, 100, 1024, 4096, 2, 31, 1, 1, 1};
        assert(handle.prepare(config, soon()).ok());
        const storage::SegmentSpec spec{{sizeof(spec)}, 1, {"/unused/compat", 14}};
        assert(handle.open_segment(spec, soon()).ok());
        const std::array<std::uint8_t, 3> payload{3, 2, 1};
        std::array<storage::Record, 2> records{};
        for (std::size_t i = 0; i < records.size(); ++i) {
            auto& record = records[i];
            record.header.struct_size = sizeof(record);
            record.stream_id = 42;
            record.descriptor_id = 5;
            record.acceptance_sequence = i + 1;
            record.system_boot_id = 1;
            record.clock_epoch_id = 1;
            record.payload = {payload.data(), payload.size()};
        }
        auto written = handle.write_batch({records.data(), records.size()}, soon());
        assert(written.status.ok() && written.batch_result.confirmed_prefix == 2);
        written = handle.write_batch({records.data(), records.size()}, soon());
        assert(
            written.status.code() == rodaq::StatusCode::Unavailable &&
            written.batch_result.confirmed_prefix == 1 &&
            written.batch_result.tail_state == RODAQ_STORAGE_TAIL_UNKNOWN);
        assert(handle.segment_usage().value().physical_bytes == 9);
        // ABI-only fixture: this validates owning output, not a normal artifact commit.
        auto sealed = handle.seal_segment(soon());
        assert(
            sealed.ok() && sealed.value().files.size() == 1 &&
            sealed.value().total_physical_bytes == 9);
        assert(sealed.value().files[0].relative_path == "compat.bin");
        assert(handle.quiesce(soon()).ok());
    }
    assert(instance.close(soon()).ok());
    assert(manager.shutdown(soon()).ok());
    assert(!manager.restart_required());
}
