#include "pipeline/recording_instance.hpp"
#include "pipeline/pipeline_writer.hpp"
#include "artifact/segment_committer.hpp"
#include "../artifact/manifest_fixture.hpp"

#include <rodaq/acquisition/interface.hpp>
#include <rodaq/acquisition/service.hpp>
#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/registry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <optional>
#include <limits>
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
        for (const auto& entry : fs::recursive_directory_iterator(root_ / "packages")) {
            fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::owner_exec);
        }
        fs::permissions(root_ / "packages", fs::perms::owner_read | fs::perms::owner_exec);
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
        fs::create_directory(root_ / "time");
        fs::create_directory(root_ / "volume");
        fs::create_directory(root_ / "ledger");
        const auto boot = rodaq::time::parse_boot_identity("11234567-89ab-4cde-8fab-0123456789ab");
        ASSERT_TRUE(boot.ok());
        auto clock = rodaq::time::ClockCorrelation::create(
            root_ / "time", boot.value(), {100, 1000, 1},
            {rodaq::time::BootArtifactState::Absent, {}}, {1000000, 1},
            [this](auto) { return fail_clock_persistence_ ? EIO : 0; });
        ASSERT_TRUE(clock.ok());
        clock_ = std::move(clock.value());
        auto volume =
            artifact::ManagedVolume::open(root_ / "volume", artifact::VolumeOpenMode::Initialize);
        ASSERT_TRUE(volume.ok());
        volume_ = std::move(volume.value());
        auto ledger =
            artifact::LedgerStore::open(root_ / "ledger", artifact::LedgerOpenMode::Initialize, 8);
        ASSERT_TRUE(ledger.ok());
        ledger_ = std::move(ledger.value());
        capacity_.emplace(std::move(
            artifact::VolumeCapacity::create({1000000, 1000000, 100, 10000, 8, 8, 16}).value()));
        auto probe = capacity_->begin_space_sample(100);
        ASSERT_TRUE(probe.ok());
        ASSERT_TRUE(capacity_->finish_space_sample(probe.value(), 1000000, 100).ok());
    }

    void TearDown() override {
        release_prepare();
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
        ledger_.reset();
        clock_.reset();
        volume_.reset();
        std::error_code error;
        fs::permissions(root_ / "packages", fs::perms::owner_all, error);
        for (const auto& entry : fs::recursive_directory_iterator(root_ / "packages", error)) {
            fs::permissions(entry.path(), fs::perms::owner_all, error);
        }
        fs::remove_all(root_, error);
    }

    void release_prepare() {
        if (!released_once_) {
            released_once_ = true;
            released_.set_value();
        }
    }

    rodaq::storage::PrepareConfig envelope() const {
        return {
            {sizeof(rodaq::storage::PrepareConfig)},
            8,
            24,
            3,
            8,
            100,
            800,
            100000,
            1,
            32,
            clock_->anchor().boot_id,
            100,
            1000};
    }

    void prepare(std::uint32_t callbacks = 2) {
        const auto identity = rodaq::test::manifest_identity();
        pipeline::InstanceLimits limits;
        limits.records = 32;
        limits.logical_bytes = 256;
        limits.payloads = {16384, 256, 8, 32};
        limits.queue_capacity = 16;
        limits.callback_concurrency = callbacks;
        limits.sources_per_stream = 1;
        auto state = pipeline::RecordingInstance::create(
            {identity.task_id, identity.pipeline_id,
             pipeline::parse_instance_id(identity.instance_id).value(), 7},
            {{1, "/first", 8, {1, 1001}, {1, 1001}}, {2, "/second", 8, {1, 1001}, {1, 1001}}},
            limits, clock_,
            [this](auto segment, auto descriptor, auto now, auto deadline) {
                ++native_prepares_;
                auto prepared = writer_->prepare_descriptor(
                    std::move(segment), std::move(descriptor), now, deadline);
                if (delay_prepare_) {
                    entered_.set_value();
                    permission_.wait();
                }
                return prepared;
            },
            &sample);
        ASSERT_TRUE(state.ok());
        state_ = std::move(state.value());
        plugin::ServiceRegistry empty;
        ASSERT_TRUE(empty.freeze().ok());
        auto storage = storage_->create_instance(empty, "{}");
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
        manifest.playback_anchor = clock_->anchor();
        manifest.acquisition = acquisition_->binary();
        manifest.storage = storage_->binary();
        auto registration = ledger_->reserve_registration(7);
        ASSERT_TRUE(registration.ok());
        artifact::ManifestLimits manifest_limits;
        manifest_limits.max_bytes = 16384;
        auto segment = pipeline::RecordingSegment::prepare(
            std::move(manifest), {1, 2}, envelope(), writer_->capacity_model(), manifest_limits,
            *capacity_, std::move(registration.value()), 100);
        ASSERT_TRUE(segment.ok());
        segment_ = std::move(segment.value());
        state_->streams.find(1)->segment = segment_;
        state_->streams.find(2)->segment = segment_;
    }

    acquisition::DescriptorProposal proposal(
        std::uint64_t stream = 1, std::string_view schema = "{}") const {
        acquisition::DescriptorProposal result{};
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

    acquisition::RecordAttempt attempt(
        acquisition::DescriptorToken token, std::uint64_t stream = 1) const {
        acquisition::RecordAttempt result{};
        result.header.struct_size = sizeof(result);
        result.stream_id = stream;
        result.descriptor_token = token;
        result.payload = {{sizeof(rodaq_payload_t)}, payload_.data(), payload_.size()};
        return result;
    }

    void open() {
        // Component gate only: formal TaskService must supply durable full-set
        // OpenAuthorization before these two calls in the final control chain.
        ASSERT_TRUE(state_->freeze_open_timeline(200, 0).ok());
        ASSERT_EQ(
            state_->gate.open_admission(state_->identity.instance_id, 7),
            pipeline::GateOpenResult::Opened);
    }

    void close() {
        ASSERT_TRUE(state_->gate.close_and_capture(state_->identity.instance_id).ok());
        segment_->close_admission();
    }

    fs::path root_;
    std::optional<plugin::PluginManager> manager_;
    std::optional<plugin::PluginLibrary> storage_;
    std::optional<plugin::PluginLibrary> acquisition_;
    std::optional<plugin::PluginInstance> storage_instance_;
    std::optional<plugin::PluginInstance> source_instance_;
    std::optional<acquisition::Handle> source_;
    std::shared_ptr<rodaq::time::ClockCorrelation> clock_;
    std::shared_ptr<artifact::ManagedVolume> volume_;
    std::unique_ptr<artifact::LedgerStore> ledger_;
    std::optional<artifact::VolumeCapacity> capacity_;
    std::shared_ptr<pipeline::RecordingInstance> state_;
    std::unique_ptr<pipeline::PipelineWriter> writer_;
    std::shared_ptr<pipeline::RecordingSegment> segment_;
    std::array<std::uint8_t, 4> payload_{1, 2, 3, 4};
    bool delay_prepare_{false};
    bool fail_clock_persistence_{false};
    bool released_once_{false};
    std::uint32_t native_prepares_{0};
    std::promise<void> entered_;
    std::promise<void> released_;
    std::shared_future<void> permission_{released_.get_future().share()};
};

TEST_F(
    RecordingInstanceTest,
    InitialAndChangedDescriptorsBindOnlyAfterActualNativePrepareAndUnchangedFactsReuseToken) {
    prepare();
    ASSERT_TRUE(state_);
    ASSERT_TRUE(segment_);
    auto first = state_->propose_descriptor(proposal(), soon());
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(native_prepares_, 1);
    EXPECT_FALSE(state_->gate.snapshot().open);
    auto duplicate = state_->propose_descriptor(proposal(), soon());
    ASSERT_TRUE(duplicate.ok());
    EXPECT_EQ(duplicate.value().value, first.value().value);
    EXPECT_EQ(native_prepares_, 1);
    EXPECT_EQ(state_->streams.find(1)->descriptor_ids.snapshot().active_remaining, 999);
    open();
    EXPECT_EQ(
        state_->submit_record(attempt(first.value())).value().disposition, RODAQ_SUBMIT_ACCEPTED);
    auto changed = state_->propose_descriptor(proposal(1, R"({"v":2})"), soon());
    ASSERT_TRUE(changed.ok());
    EXPECT_EQ(native_prepares_, 2);
    EXPECT_EQ(
        state_->submit_record(attempt(first.value())).value().disposition,
        RODAQ_SUBMIT_STALE_DESCRIPTOR);
    EXPECT_EQ(
        state_->submit_record(attempt(changed.value())).value().disposition, RODAQ_SUBMIT_ACCEPTED);
    ASSERT_TRUE(writer_->write_available(state_->queue, segment_, soon()).ok());
    EXPECT_EQ(segment_->metadata.snapshot().written, 2);
    close();
    EXPECT_TRUE(writer_->seal(segment_, soon()).ok());
}

TEST_F(
    RecordingInstanceTest, FreshReceiveClocksRejectUnobservedJumpBeforeSequenceAndThenUseNewEpoch) {
    prepare();
    ASSERT_TRUE(state_);
    ASSERT_TRUE(segment_);
    auto token = state_->propose_descriptor(proposal(), soon());
    ASSERT_TRUE(token.ok());
    open();
    receive_mono = 300;
    receive_real = 1200;
    ASSERT_EQ(
        state_->submit_record(attempt(token.value())).value().disposition, RODAQ_SUBMIT_ACCEPTED);
    receive_mono = 400;
    receive_real = 5000000;
    EXPECT_EQ(
        state_->submit_record(attempt(token.value())).value().disposition,
        RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    EXPECT_EQ(state_->streams.find(1)->sequences.snapshot().active_remaining, 999);
    ASSERT_TRUE(clock_->observe(sample().value()).ok());
    EXPECT_EQ(
        state_->submit_record(attempt(token.value())).value().disposition, RODAQ_SUBMIT_ACCEPTED);
    auto first = state_->queue.pop();
    auto second = state_->queue.pop();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->received.sample.monotonic_ns, 300);
    EXPECT_EQ(first->received.sample.realtime_ns, 1200);
    EXPECT_NE(first->received.epoch_id, second->received.epoch_id);
    EXPECT_EQ(second->received.sample.realtime_ns, 5000000);
    EXPECT_EQ(second->acceptance_sequence, 2);
    EXPECT_TRUE(first->settlement.finish(pipeline::RecordSettlementKind::KnownFailed, 0).ok());
    EXPECT_TRUE(second->settlement.finish(pipeline::RecordSettlementKind::KnownFailed, 0).ok());
    EXPECT_EQ(state_->callback_facts().value().clock_rejections, 1);
}

TEST_F(RecordingInstanceTest, ActualDescriptorReturnAfterFenceCannotBindOrReopenOldInstance) {
    prepare();
    ASSERT_TRUE(state_);
    ASSERT_TRUE(segment_);
    delay_prepare_ = true;
    auto work = std::async(std::launch::async, [owned = state_, this] {
        return owned->propose_descriptor(proposal(), plugin::Deadline::after(1ms).value());
    });
    entered_.get_future().wait();
    EXPECT_EQ(work.wait_for(5ms), std::future_status::timeout);
    EXPECT_EQ(state_->callback_facts().value().callbacks, 1);
    EXPECT_TRUE(state_->streams.find(1)->descriptor_proposal_inflight);
    EXPECT_EQ(state_->propose_descriptor(proposal(), soon()).status().code(), StatusCode::Conflict);
    ASSERT_TRUE(state_->gate.establish_control_fence(state_->identity.instance_id).ok());
    release_prepare();
    ASSERT_FALSE(work.get().ok());
    EXPECT_FALSE(state_->streams.find(1)->active_descriptor);
    EXPECT_FALSE(state_->streams.find(1)->descriptor_proposal_inflight);
    EXPECT_EQ(state_->callback_facts().value().callbacks, 0);
    EXPECT_EQ(
        state_->gate.open_admission(state_->identity.instance_id, 7),
        pipeline::GateOpenResult::Obsolete);
}

TEST_F(RecordingInstanceTest, CallbackQuotaRetainsInflightDescriptorOwnerAndRejectsExtraCallback) {
    prepare(1);
    ASSERT_TRUE(state_);
    ASSERT_TRUE(segment_);
    auto token = state_->propose_descriptor(proposal(), soon());
    ASSERT_TRUE(token.ok());
    delay_prepare_ = true;
    auto work = std::async(std::launch::async, [owned = state_, this] {
        return owned->propose_descriptor(proposal(2), soon());
    });
    entered_.get_future().wait();
    acquisition::AcquisitionEvent event{};
    event.header.struct_size = sizeof(event);
    event.kind = RODAQ_ACQUISITION_EVENT_TRANSPORT_READY;
    EXPECT_EQ(
        state_->report_acquisition_event(event).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(state_->callback_facts().value().callbacks, 1);
    EXPECT_EQ(state_->callback_facts().value().high_water_callbacks, 1);
    EXPECT_EQ(
        state_->submit_record(attempt(token.value())).value().disposition,
        RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    EXPECT_EQ(
        state_->callback_facts().value().streams[0].submissions[RODAQ_SUBMIT_RESOURCE_EXHAUSTED],
        1);
    release_prepare();
    EXPECT_TRUE(work.get().ok());
    EXPECT_EQ(state_->callback_facts().value().callback_rejections, 2);
}

TEST_F(RecordingInstanceTest, SourceCardinalityIsBoundedAndLateRecordDoesNotResurrectLostSource) {
    prepare();
    ASSERT_TRUE(state_);
    ASSERT_TRUE(segment_);
    auto token = state_->propose_descriptor(proposal(), soon());
    ASSERT_TRUE(token.ok());
    acquisition::SourceEvent event{};
    event.header.struct_size = sizeof(event);
    event.stream_id = 1;
    event.has_source_id = 1;
    event.source_id.kind = RODAQ_SOURCE_ID_RTPS_GUID;
    event.source_id.size = 16;
    event.source_id.data[0] = 1;
    event.kind = RODAQ_SOURCE_EVENT_DISCOVERED;
    ASSERT_TRUE(state_->report_source_event(event).ok());
    event.source_id.data[0] = 2;
    ASSERT_TRUE(state_->report_source_event(event).ok());
    auto facts = state_->callback_facts().value();
    EXPECT_EQ(facts.streams[0].active_sources, 1);
    EXPECT_EQ(facts.streams[0].source_overflow, 1);
    EXPECT_FALSE(facts.streams[0].evidence_complete);
    event.source_id.data[0] = 1;
    event.kind = RODAQ_SOURCE_EVENT_LOST;
    ASSERT_TRUE(state_->report_source_event(event).ok());
    open();
    auto late = attempt(token.value());
    late.source_id = event.source_id;
    late.has_source_id = 1;
    EXPECT_EQ(state_->submit_record(late).value().disposition, RODAQ_SUBMIT_ACCEPTED);
    EXPECT_EQ(state_->callback_facts().value().streams[0].active_sources, 0);
    ASSERT_TRUE(writer_->write_available(state_->queue, segment_, soon()).ok());
    close();
    EXPECT_TRUE(writer_->seal(segment_, soon()).ok());
}

TEST_F(
    RecordingInstanceTest,
    RealAcquisitionCallbacksReachCoreCopyWriterAndDurableArtifactThroughSdkServices) {
    prepare();
    ASSERT_TRUE(state_);
    ASSERT_TRUE(segment_);
    {
        plugin::ServiceRegistry services;
        ASSERT_TRUE(services.add_shared<acquisition::Service>(state_).ok());
        ASSERT_TRUE(services.freeze().ok());
        auto instance = acquisition_->create_instance(
            services, R"({"streams":["/first","/second"],"period_us":1000})");
        ASSERT_TRUE(instance.ok());
        source_instance_.emplace(std::move(instance.value()));
    }
    auto source = source_instance_->query<acquisition::Interface>();
    ASSERT_TRUE(source.ok());
    source_.emplace(std::move(source.value()));
    const auto& bindings = state_->streams.bindings();
    const acquisition::PrepareConfig config{{sizeof(config)}, bindings.data(), 2, 1};
    ASSERT_TRUE(source_->prepare(config, soon()).ok());
    ASSERT_TRUE(source_->start(soon()).ok());
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (state_->callback_facts().value().streams[0].submissions[RODAQ_SUBMIT_GATE_CLOSED] == 0 &&
           std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(1ms);
    }
    open();
    std::weak_ptr<pipeline::RecordingInstance> retained = state_;
    state_.reset();  // Frozen SDK service backing owns the actual callback target.
    ASSERT_FALSE(retained.expired());
    state_ = retained.lock();
    while (state_->queue.snapshot().queued_records < 4 &&
           std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_GE(state_->queue.snapshot().queued_records, 4);
    close();
    ASSERT_TRUE(source_->quiesce(soon()).ok());
    const auto accepted = state_->gate.snapshot().accepted_records;
    while (state_->queue.snapshot().queued_records != 0) {
        auto written = writer_->write_available(state_->queue, segment_, soon());
        ASSERT_TRUE(written.ok());
        ASSERT_EQ(written.value().failure, StatusCode::Ok);
    }
    EXPECT_EQ(state_->records.snapshot().records, 0);
    EXPECT_EQ(state_->payloads.snapshot().live_buffers, 0);
    EXPECT_EQ(segment_->metadata.snapshot().written, accepted);
    EXPECT_EQ(state_->callback_facts().value().callbacks, 0);
    auto sealed = writer_->seal(segment_, soon());
    ASSERT_TRUE(sealed.ok());
    auto files =
        artifact::SealedFiles::open(volume_, 7, sealed.value(), envelope(), 4 * 1024 * 1024);
    ASSERT_TRUE(files.ok());
    auto manifest = segment_->metadata.capture_manifest(files.value()->evidence());
    ASSERT_TRUE(manifest.ok());
    EXPECT_EQ(manifest.value().identity.acquisition.sha256, acquisition_->binary().sha256);
    auto reservation = segment_->capacity.handoff_sealed(files.value()->storage_bytes());
    ASSERT_TRUE(reservation.ok());
    auto commit = artifact::SegmentCommitter::create(
        files.value(), manifest.value(), {}, reservation.value());
    ASSERT_TRUE(commit.ok());
    ASSERT_TRUE(commit.value()->commit(*ledger_).ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committed);
    EXPECT_TRUE(commit.value()->take_committed_charge().ok());
}

TEST_F(RecordingInstanceTest, FailedTimeAuthorityFencesAdmissionWithoutConsumingRecordSequence) {
    prepare();
    ASSERT_TRUE(state_);
    ASSERT_TRUE(segment_);
    auto token = state_->propose_descriptor(proposal(), soon());
    ASSERT_TRUE(token.ok());
    open();
    receive_mono = 300;
    receive_real = 5000000;
    fail_clock_persistence_ = true;
    EXPECT_FALSE(clock_->observe(sample().value()).ok());
    EXPECT_EQ(
        state_->submit_record(attempt(token.value())).value().disposition,
        RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    EXPECT_TRUE(state_->gate.admission_fenced());
    EXPECT_EQ(state_->gate.snapshot().accepted_records, 0);
    EXPECT_EQ(state_->streams.find(1)->sequences.snapshot().active_remaining, 1000);
    EXPECT_EQ(state_->records.snapshot().records, 0);
    EXPECT_TRUE(state_->callback_facts().value().terminal_failure);
}

TEST_F(
    RecordingInstanceTest,
    NativeReportedCountsSaturateWithoutWrapAndTerminalFailureFencesTheInstance) {
    prepare();
    ASSERT_TRUE(state_);
    ASSERT_TRUE(segment_);
    acquisition::AcquisitionEvent event{};
    event.header.struct_size = sizeof(event);
    event.kind = RODAQ_ACQUISITION_EVENT_OVERFLOW;
    event.stream_id = 1;
    event.has_stream_id = 1;
    event.count = 7;
    ASSERT_TRUE(state_->report_acquisition_event(event).ok());
    auto facts = state_->callback_facts().value();
    EXPECT_EQ(facts.streams[0].acquisition_events[event.kind], 1);
    EXPECT_EQ(facts.streams[0].reported_counts[event.kind], 7);
    event.count = std::numeric_limits<std::uint64_t>::max();
    ASSERT_TRUE(state_->report_acquisition_event(event).ok());
    facts = state_->callback_facts().value();
    EXPECT_EQ(
        facts.streams[0].reported_counts[event.kind], std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(facts.evidence_complete);
    EXPECT_FALSE(facts.streams[0].evidence_complete);
    event = {};
    event.header.struct_size = sizeof(event);
    event.kind = RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE;
    event.status = RODAQ_STATUS_UNAVAILABLE;
    ASSERT_TRUE(state_->report_acquisition_event(event).ok());
    EXPECT_TRUE(state_->gate.admission_fenced());
    EXPECT_TRUE(state_->callback_facts().value().terminal_failure);
}

}  // namespace
