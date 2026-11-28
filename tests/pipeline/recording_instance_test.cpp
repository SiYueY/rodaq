#include "pipeline/recording_instance.hpp"
#include "pipeline/pipeline_writer.hpp"
#include "../artifact/manifest_fixture.hpp"

#include <rodaq/acquisition/interface.hpp>
#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace {

namespace pipeline = rodaq::pipeline;
namespace artifact = rodaq::artifact;
namespace plugin = rodaq::plugin;
namespace acquisition = rodaq::acquisition;
namespace fs = std::filesystem;
using rodaq::StatusCode;
using namespace std::chrono_literals;

std::atomic<std::uint64_t> receive_mono{200};
std::atomic<std::int64_t> receive_real{1100};

rodaq::Result<rodaq::time::ClockSample> sample() {
    return rodaq::time::ClockSample{receive_mono.load(), receive_real.load(), 1};
}

plugin::Deadline soon() { return plugin::Deadline::after(2s).value(); }

class RecordingInstanceTest : public testing::Test {
protected:
    void SetUp() override {
        receive_mono = 200;
        receive_real = 1100;
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-instance-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        fs::create_directory(root_ / "packages");
        fs::copy(
            RODAQ_INSTANCE_STORAGE_PACKAGE, root_ / "packages" / "storage",
            fs::copy_options::recursive);
        fs::copy(
            RODAQ_INSTANCE_ACQUISITION_PACKAGE, root_ / "packages" / "acquisition",
            fs::copy_options::recursive);
        plugin::PluginManagerOptions options;
        options.search_paths = {root_ / "packages"};
        options.max_loaded_plugins = 2;
        options.max_instances = 2;
        options.max_config_size = 4096;
        options.shutdown_timeout = 2s;
        auto manager = plugin::PluginManager::create(std::move(options));
        ASSERT_TRUE(manager.ok());
        manager_.emplace(std::move(manager.value()));
        auto storage = manager_->load("rodaq.fake.storage", "1.0.0");
        auto acquisition = manager_->load("rodaq.fake.acquisition", "1.0.0");
        ASSERT_TRUE(storage.ok());
        ASSERT_TRUE(acquisition.ok());
        storage_.emplace(std::move(storage.value()));
        acquisition_.emplace(std::move(acquisition.value()));
        fs::create_directory(root_ / "volume");
        auto volume =
            artifact::ManagedVolume::open(root_ / "volume", artifact::VolumeOpenMode::Initialize);
        ASSERT_TRUE(volume.ok());
        volume_ = std::move(volume.value());
        capacity_.emplace(std::move(
            artifact::VolumeCapacity::create({1000000, 1000000, 100, 10000, 8, 8, 16}).value()));
        auto probe = capacity_->begin_space_sample(100);
        ASSERT_TRUE(probe.ok());
        ASSERT_TRUE(capacity_->finish_space_sample(probe.value(), 1000000, 100).ok());
    }

    void TearDown() override {
        if (source_) {
            (void)source_->quiesce(soon());
        }
        source_.reset();
        if (source_instance_) {
            (void)source_instance_->close(soon());
        }
        source_instance_.reset();
        if (writer_) {
            (void)writer_->quiesce(soon());
        }
        writer_.reset();
        if (storage_instance_) {
            (void)storage_instance_->close(soon());
        }
        storage_instance_.reset();
        acquisition_.reset();
        storage_.reset();
        if (manager_) {
            (void)manager_->shutdown(soon());
        }
        manager_.reset();
        segment_.reset();
        state_.reset();
        volume_.reset();
        std::error_code error;
        fs::remove_all(root_, error);
    }

    rodaq::storage::PrepareConfig envelope() const {
        return {{sizeof(rodaq::storage::PrepareConfig)}, 8, 24, 3, 8, 100, 800, 100000, 1, 32};
    }

    void prepare(std::uint32_t callbacks = 2) {
        const auto identity = rodaq::test::manifest_identity();
        pipeline::InstanceLimits limits;
        limits.records = 32;
        limits.logical_bytes = 256;
        limits.payloads = {16384, 256, 8, 32};
        limits.queue_capacity = 16;
        limits.callback_concurrency = callbacks;
        auto state = pipeline::RecordingInstance::create(
            {identity.task_id, identity.pipeline_id,
             pipeline::parse_instance_id(identity.instance_id).value(), 7},
            {{1, "/first", 8}, {2, "/second", 8}}, limits,
            [this](auto segment, auto descriptor, auto deadline) {
                ++native_prepares_;
                return writer_->prepare_descriptor(
                    std::move(segment), std::move(descriptor), deadline);
            },
            &sample);
        ASSERT_TRUE(state.ok());
        state_ = std::move(state.value());

        auto storage = storage_->create_instance("{}");
        ASSERT_TRUE(storage.ok());
        storage_instance_.emplace(std::move(storage.value()));
        auto handle = storage_instance_->query<rodaq::storage::Interface>();
        ASSERT_TRUE(handle.ok());
        auto writer = pipeline::PipelineWriter::prepare(
            std::move(handle.value()), envelope(), volume_, state_->gate,
            state_->identity.instance_id, 16, soon());
        ASSERT_TRUE(writer.ok());
        writer_ = std::move(writer.value());

        auto manifest = identity;
        manifest.volume_id = volume_->identity();
        manifest.playback_anchor = state_->playback_anchor();
        manifest.acquisition = acquisition_->info();
        manifest.storage = storage_->info();
        artifact::ManifestLimits manifest_limits;
        manifest_limits.max_bytes = 16384;
        auto segment = pipeline::RecordingSegment::prepare(
            std::move(manifest), {1, 2}, envelope(), manifest_limits, *capacity_, 65536, 100);
        ASSERT_TRUE(segment.ok());
        segment_ = std::move(segment.value());
        state_->streams.find(1)->segment = segment_;
        state_->streams.find(2)->segment = segment_;
    }

    acquisition::Descriptor make_descriptor(
        std::uint64_t stream = 1, std::string_view schema = "{}") const {
        acquisition::Descriptor result{};
        result.header.struct_size = sizeof(result);
        result.stream_id = stream;
        result.data_profile = RODAQ_DATA_PROFILE_GENERIC;
        result.channel_name = {"/data", 5};
        result.message_encoding = {"raw", 3};
        result.schema_encoding = {"jsonschema", 10};
        result.schema_name = {"schema", 6};
        result.schema = {
            reinterpret_cast<const std::uint8_t*>(schema.data()),
            static_cast<std::uint32_t>(schema.size())};
        return result;
    }

    acquisition::RecordAttempt make_attempt(
        rodaq_descriptor_id_t descriptor_id, std::uint64_t stream = 1) const {
        acquisition::RecordAttempt result{};
        result.header.struct_size = sizeof(result);
        result.stream_id = stream;
        result.descriptor_id = descriptor_id;
        result.payload = {payload_.data(), static_cast<std::uint32_t>(payload_.size())};
        return result;
    }

    fs::path root_;
    std::optional<plugin::PluginManager> manager_;
    std::optional<plugin::PluginLibrary> storage_;
    std::optional<plugin::PluginLibrary> acquisition_;
    std::optional<plugin::PluginInstance> storage_instance_;
    std::optional<plugin::PluginInstance> source_instance_;
    std::optional<acquisition::Handle> source_;
    std::shared_ptr<artifact::ManagedVolume> volume_;
    std::optional<artifact::VolumeCapacity> capacity_;
    std::shared_ptr<pipeline::RecordingInstance> state_;
    std::unique_ptr<pipeline::PipelineWriter> writer_;
    std::shared_ptr<pipeline::RecordingSegment> segment_;
    std::array<std::uint8_t, 4> payload_{1, 2, 3, 4};
    std::uint32_t native_prepares_{0};
};

TEST_F(RecordingInstanceTest, BindOnceDescriptorRejectsSecondBindAndSupportsCleanSubmission) {
    prepare();
    const auto d1 = state_->prepare_descriptor(1, make_descriptor(1), soon());
    ASSERT_TRUE(d1.ok()) << d1.status().message();
    EXPECT_GT(d1.value(), 0ULL);
    EXPECT_EQ(native_prepares_, 1U);

    // Bind-once: a second prepare_descriptor on the same stream must fail-closed.
    EXPECT_EQ(
        state_->prepare_descriptor(1, make_descriptor(1, "{\"v\":2}"), soon()).status().code(),
        StatusCode::Conflict);
    EXPECT_EQ(native_prepares_, 1U);

    // Before gate open, submissions are rejected with GATE_CLOSED.
    ASSERT_TRUE(state_->freeze_open_timeline(200, 0).ok());
    EXPECT_EQ(state_->submit_record(make_attempt(d1.value(), 1)).value(), RODAQ_SUBMIT_GATE_CLOSED);

    ASSERT_EQ(
        state_->gate.open_admission(state_->identity.instance_id, 7),
        pipeline::GateOpenResult::Opened);
    EXPECT_EQ(state_->submit_record(make_attempt(d1.value(), 1)).value(), RODAQ_SUBMIT_ACCEPTED);
    EXPECT_EQ(state_->queue.snapshot().queued_records, 1U);

    // Wrong descriptor_id is rejected as STALE_DESCRIPTOR.
    EXPECT_EQ(
        state_->submit_record(make_attempt(d1.value() + 99, 1)).value(),
        RODAQ_SUBMIT_STALE_DESCRIPTOR);

    ASSERT_TRUE(state_->establish_control_fence(250).ok());
    EXPECT_EQ(state_->submit_record(make_attempt(d1.value(), 1)).value(), RODAQ_SUBMIT_GATE_CLOSED);
    auto boundary = state_->recording_time_boundary();
    ASSERT_TRUE(boundary.ok());
    ASSERT_TRUE(boundary.value().has_value());
    EXPECT_GE(*boundary.value(), 50ULL);
}

TEST_F(RecordingInstanceTest, ReadinessPoliciesDistinguishEntryReadyAndSourceReady) {
    prepare();
    const std::vector<rodaq::ReadinessPolicy> entry_policies{
        rodaq::ReadinessPolicy::EntryReady, rodaq::ReadinessPolicy::EntryReady};
    const std::vector<rodaq::ReadinessPolicy> source_policies{
        rodaq::ReadinessPolicy::SourceReady, rodaq::ReadinessPolicy::EntryReady};
    EXPECT_FALSE(state_->ready(entry_policies));

    ASSERT_TRUE(state_->prepare_descriptor(1, make_descriptor(1), soon()).ok());
    ASSERT_TRUE(state_->prepare_descriptor(2, make_descriptor(2), soon()).ok());

    acquisition::AcquisitionEvent transport{};
    transport.header.struct_size = sizeof(transport);
    transport.kind = RODAQ_ACQUISITION_EVENT_TRANSPORT_READY;
    ASSERT_TRUE(state_->report_acquisition_event(transport).ok());

    EXPECT_TRUE(state_->ready(entry_policies));
    EXPECT_FALSE(state_->ready(source_policies));

    acquisition::AcquisitionEvent stream_ready{};
    stream_ready.header.struct_size = sizeof(stream_ready);
    stream_ready.kind = RODAQ_ACQUISITION_EVENT_STREAM_READY;
    stream_ready.has_stream_id = 1;
    stream_ready.stream_id = 1;
    ASSERT_TRUE(state_->report_acquisition_event(stream_ready).ok());
    EXPECT_TRUE(state_->ready(source_policies));
}

TEST_F(RecordingInstanceTest, FakeAcquisitionCallbacksDriveEndToEndIngestion) {
    prepare();
    auto acq_inst = acquisition_->create_instance(
        R"({"streams":["/first","/second"],"period_us":1000,"payload_bytes":4})");
    ASSERT_TRUE(acq_inst.ok()) << acq_inst.status().message();
    source_instance_.emplace(std::move(acq_inst.value()));
    auto acq_handle = source_instance_->query<acquisition::Interface>();
    ASSERT_TRUE(acq_handle.ok());
    source_.emplace(std::move(acq_handle.value()));

    const auto& bindings = state_->streams.bindings();
    acquisition::PrepareConfig cfg{
        {sizeof(cfg)}, bindings.data(), static_cast<std::uint32_t>(bindings.size()), 2};
    const auto callbacks = state_->callbacks_table(soon());
    ASSERT_TRUE(source_->prepare(cfg, callbacks, soon()).ok());
    ASSERT_TRUE(state_->freeze_open_timeline(200, 0).ok());
    ASSERT_EQ(
        state_->gate.open_admission(state_->identity.instance_id, 7),
        pipeline::GateOpenResult::Opened);
    ASSERT_TRUE(source_->start(soon()).ok());

    const auto until = std::chrono::steady_clock::now() + 2s;
    while (state_->queue.snapshot().queued_records < 2 &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_TRUE(source_->quiesce(soon()).ok());
    EXPECT_GE(state_->queue.snapshot().queued_records, 2U);

    // Drain & settle everything cleanly.
    auto step = writer_->write_available(state_->queue, segment_, soon());
    ASSERT_TRUE(step.ok());
    EXPECT_GE(step.value().written, 2U);
    ASSERT_TRUE(state_->gate.close_and_capture(state_->identity.instance_id).ok());
    segment_->close_admission();
}

}  // namespace
