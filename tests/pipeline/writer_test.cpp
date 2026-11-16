#include "pipeline/pipeline_writer.hpp"
#include "pipeline/admission.hpp"
#include "pipeline/descriptor_registry.hpp"
#include "pipeline/stream_table.hpp"
#include "artifact/segment_committer.hpp"
#include "../artifact/manifest_fixture.hpp"

#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/registry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <thread>

namespace {

namespace fs = std::filesystem;
namespace pipeline = rodaq::pipeline;
namespace artifact = rodaq::artifact;
namespace plugin = rodaq::plugin;
using rodaq::StatusCode;
using namespace std::chrono_literals;

plugin::Deadline soon() { return plugin::Deadline::after(2s).value(); }

// Component integration uses actual SDK/backend/Admission/Artifact owners.
// It does not substitute for the formal TaskService/OpenAuthorization gate.
class PipelineWriterTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-writer-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        fs::copy(RODAQ_WRITER_STORAGE_PACKAGE, root_ / "package", fs::copy_options::recursive);
        for (const auto& item : fs::recursive_directory_iterator(root_ / "package")) {
            fs::permissions(item.path(), fs::perms::owner_read | fs::perms::owner_exec);
        }
        fs::permissions(root_ / "package", fs::perms::owner_read | fs::perms::owner_exec);
        plugin::PluginManagerOptions options;
        options.search_paths = {root_ / "package"};
        options.max_loaded_plugins = 1;
        options.max_instances = 1;
        options.max_config_size = 1024;
        options.shutdown_timeout = 2s;
        auto manager = plugin::PluginManager::create(std::move(options));
        ASSERT_TRUE(manager.ok());
        manager_.emplace(std::move(manager.value()));
        auto library = manager_->load("rodaq.fake.storage", "1.0.0");
        ASSERT_TRUE(library.ok());
        library_.emplace(std::move(library.value()));
        ASSERT_TRUE(services_.freeze().ok());
        fs::create_directory(root_ / "volume");
        fs::create_directory(root_ / "ledger");
        auto managed =
            artifact::ManagedVolume::open(root_ / "volume", artifact::VolumeOpenMode::Initialize);
        ASSERT_TRUE(managed.ok());
        volume_ = std::move(managed.value());
        auto ledger =
            artifact::LedgerStore::open(root_ / "ledger", artifact::LedgerOpenMode::Initialize, 16);
        ASSERT_TRUE(ledger.ok());
        ledger_ = std::move(ledger.value());
        auto capacity = artifact::VolumeCapacity::create({32000000, 8000000, 100, 10000, 8, 8, 16});
        ASSERT_TRUE(capacity.ok());
        capacity_.emplace(std::move(capacity.value()));
        auto probe = capacity_->begin_space_sample(100);
        ASSERT_TRUE(probe.ok());
        ASSERT_TRUE(capacity_->finish_space_sample(probe.value(), 32000000, 100).ok());
        id_ = pipeline::parse_instance_id("21234567-89ab-4cde-8fab-0123456789ab").value();
        gate_.emplace(id_, 7);
        auto streams = pipeline::StreamTable::create(
            {{1, "/first", 8, {1, 1001}, {1, 1001}}, {2, "/second", 8, {1, 1001}, {1, 1001}}});
        ASSERT_TRUE(streams.ok());
        streams_.emplace(std::move(streams.value()));
        first_ = streams_->find(1);
        second_ = streams_->find(2);
        auto registry = pipeline::DescriptorRegistry::create({1, 2});
        ASSERT_TRUE(registry.ok());
        registry_.emplace(std::move(registry.value()));
        queue_.emplace(std::move(pipeline::RecordQueue::create(16).value()));
        budget_.emplace(std::move(pipeline::RecordBudget::create(32, 256).value()));
        pool_.emplace(std::move(pipeline::PayloadPool::create({16384, 256, 8, 32}).value()));
        const auto task = rodaq::test::manifest_identity().task_id;
        domain_.emplace(
            pipeline::AdmissionDomain{task, 3, id_, 7, *gate_, *budget_, *pool_, *queue_, 0, 200});
    }

    void TearDown() override {
        if (writer_) {
            (void)writer_->quiesce(soon());
        }
        writer_.reset();
        if (instance_) {
            (void)instance_->close(soon());
        }
        instance_.reset();
        library_.reset();
        if (manager_) {
            (void)manager_->shutdown(soon());
        }
        manager_.reset();
        if (first_) {
            first_->segment.reset();
        }
        if (second_) {
            second_->segment.reset();
        }
        segment_.reset();
        ledger_.reset();
        volume_.reset();
        std::error_code error;
        fs::permissions(root_ / "package", fs::perms::owner_all, error);
        for (const auto& item : fs::recursive_directory_iterator(root_ / "package", error)) {
            fs::permissions(item.path(), fs::perms::owner_all, error);
        }
        fs::remove_all(root_, error);
    }

    rodaq::storage::PrepareConfig envelope() const {
        return {{sizeof(rodaq::storage::PrepareConfig)},
                8,
                12,
                3,
                4,
                100,
                800,
                1000000,
                1,
                32,
                7,
                100,
                1000};
    }

    rodaq::Result<std::shared_ptr<pipeline::RecordingSegment>> new_segment(std::uint64_t id) {
        auto identity = rodaq::test::manifest_identity(id);
        identity.volume_id = volume_->identity();
        identity.storage = library_->binary();
        identity.playback_anchor = {7, 100, 1000};
        auto registration = ledger_->reserve_registration(id);
        if (!registration.ok()) {
            return registration.status();
        }
        artifact::ManifestLimits limits;
        limits.max_bytes = 16384;
        limits.max_files = 1;
        limits.max_streams = 2;
        limits.max_descriptors = 4;
        limits.max_epochs = 4;
        return pipeline::RecordingSegment::prepare(
            std::move(identity), {1, 2}, envelope(), writer_->capacity_model(), limits, *capacity_,
            std::move(registration.value()), 100);
    }

    pipeline::DescriptorRef descriptor(rodaq::StreamId stream, std::uint64_t identity = 1) {
        rodaq::acquisition::DescriptorProposal proposal{};
        proposal.header.struct_size = sizeof(proposal);
        proposal.stream_id = stream;
        proposal.data_profile = RODAQ_DATA_PROFILE_GENERIC;
        proposal.channel_name = {"/fake", 5};
        proposal.message_encoding = {"json", 4};
        auto* state = streams_->find(stream);
        if (!state) {
            ADD_FAILURE() << "Unconfigured Stream";
            return nullptr;
        }
        rodaq::Result<std::uint64_t> reserved = [&] {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->descriptor_ids.allocate();
        }();
        if (!reserved.ok()) {
            ADD_FAILURE() << "Descriptor range exhausted";
            return nullptr;
        }
        EXPECT_EQ(reserved.value(), identity);
        auto result = registry_->capture(proposal, reserved.value());
        EXPECT_TRUE(result.ok());
        return result.ok() ? std::move(result.value()) : nullptr;
    }

    void prepare(
        std::string_view config = "{}", bool initial_descriptors = true,
        std::uint32_t catalog_capacity = 8) {
        auto instance = library_->create_instance(services_, config);
        ASSERT_TRUE(instance.ok());
        instance_.emplace(std::move(instance.value()));
        auto handle = instance_->query<rodaq::storage::Interface>();
        ASSERT_TRUE(handle.ok());
        auto writer = pipeline::PipelineWriter::prepare(
            std::move(handle.value()), envelope(), volume_, *gate_, id_, catalog_capacity, soon());
        ASSERT_TRUE(writer.ok());
        writer_ = std::move(writer.value());
        auto segment = new_segment(7);
        ASSERT_TRUE(segment.ok());
        segment_ = std::move(segment.value());
        first_->active_descriptor = descriptor(1);
        second_->active_descriptor = descriptor(2);  // descriptor_id is Stream-scoped.
        first_->segment = segment_;
        second_->segment = segment_;
        if (!initial_descriptors) {
            return;
        }
        ASSERT_TRUE(
            writer_->prepare_descriptor(segment_, first_->active_descriptor, 100, soon()).ok());
        ASSERT_TRUE(
            writer_->prepare_descriptor(segment_, second_->active_descriptor, 100, soon()).ok());
        ASSERT_EQ(gate_->open_admission(id_, 7), pipeline::GateOpenResult::Opened);
    }

    void submit(pipeline::StreamAdmission& stream, std::size_t bytes = 4) {
        std::array<std::uint8_t, 8> payload{1, 2, 3, 4, 5, 6, 7, 8};
        rodaq::acquisition::RecordAttempt attempt{};
        attempt.header.struct_size = sizeof(attempt);
        attempt.stream_id = stream.stream_id;
        attempt.descriptor_token = stream.active_descriptor->token;
        attempt.payload = {{sizeof(rodaq_payload_t)}, payload.data(), bytes};
        const rodaq::time::ClockSnapshot received{
            7, 1, {200, 1100, 1}, rodaq::time::ClockEpochOrigin::Boot, {}};
        const auto accepted = pipeline::submit_record(*domain_, stream, attempt, received);
        ASSERT_TRUE(accepted.ok());
        ASSERT_EQ(accepted.value().disposition, RODAQ_SUBMIT_ACCEPTED);
        payload.fill(9);
    }

    void close() {
        ASSERT_TRUE(gate_->close_and_capture(id_).ok());
        segment_->close_admission();
    }

    rodaq::Result<std::unique_ptr<artifact::SegmentCommitter>> seal_to_commit(
        std::shared_ptr<pipeline::RecordingSegment> segment) {
        auto sealed = writer_->seal(segment, soon());
        if (!sealed.ok()) {
            return sealed.status();
        }
        auto files = artifact::SealedFiles::open(
            volume_, segment->metadata.segment_id(), sealed.value(), envelope(), 16384);
        if (!files.ok()) {
            return files.status();
        }
        auto manifest = segment->metadata.capture_manifest(files.value()->evidence());
        if (!manifest.ok()) {
            return manifest.status();
        }
        auto reservation = segment->capacity.handoff_sealed(files.value()->storage_bytes());
        if (!reservation.ok()) {
            return reservation.status();
        }
        return artifact::SegmentCommitter::create(
            files.value(), manifest.value(), {}, reservation.value());
    }

    fs::path root_;
    plugin::ServiceRegistry services_;
    std::optional<plugin::PluginManager> manager_;
    std::optional<plugin::PluginLibrary> library_;
    std::optional<plugin::PluginInstance> instance_;
    std::shared_ptr<artifact::ManagedVolume> volume_;
    std::unique_ptr<artifact::LedgerStore> ledger_;
    std::optional<artifact::VolumeCapacity> capacity_;
    pipeline::InstanceId id_;
    std::optional<pipeline::AdmissionGate> gate_;
    std::optional<pipeline::DescriptorRegistry> registry_;
    std::optional<pipeline::RecordQueue> queue_;
    std::optional<pipeline::RecordBudget> budget_;
    std::optional<pipeline::PayloadPool> pool_;
    std::optional<pipeline::AdmissionDomain> domain_;
    std::optional<pipeline::StreamTable> streams_;
    pipeline::StreamAdmission* first_{nullptr};
    pipeline::StreamAdmission* second_{nullptr};
    std::shared_ptr<pipeline::RecordingSegment> segment_;
    std::unique_ptr<pipeline::PipelineWriter> writer_;
};

TEST_F(PipelineWriterTest, AcceptedCopiesReachActualSdkStorageAndDurableLedgerCommit) {
    prepare();
    ASSERT_TRUE(writer_);
    EXPECT_FALSE(fs::exists(volume_->artifacts_directory() / "7.tmp"));
    submit(*first_);
    submit(*second_);
    submit(*first_);
    auto step = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(step.ok());
    EXPECT_EQ(step.value().written, 3);
    EXPECT_EQ(step.value().failure, StatusCode::Ok);
    EXPECT_FALSE(step.value().next_segment);
    EXPECT_EQ(budget_->snapshot().records, 0);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0);
    close();
    auto commit = seal_to_commit(segment_);
    ASSERT_TRUE(commit.ok());
    ASSERT_TRUE(commit.value()->commit(*ledger_).ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committed);
    EXPECT_EQ(capacity_->snapshot().commit_slots, 1);
    auto charge = commit.value()->take_committed_charge();
    ASSERT_TRUE(charge.ok());
    EXPECT_EQ(capacity_->snapshot().commit_slots, 0);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 1);
    std::ifstream file(volume_->artifacts_directory() / "7" / "records.fake", std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    EXPECT_EQ(bytes.substr(0, 8), "RDAQFK1\n");
    EXPECT_NE(bytes.find(std::string("\x01\x02\x03\x04", 4)), std::string::npos);
    EXPECT_EQ(bytes.find(std::string(4, '\x09')), std::string::npos);
    EXPECT_TRUE(writer_->quiesce(soon()).ok());
}

TEST_F(PipelineWriterTest, BatchByteLimitKeepsRemainingAcceptedRecordInItsOriginalQueuePosition) {
    prepare();
    ASSERT_TRUE(writer_);
    submit(*first_, 8);
    submit(*first_, 8);
    auto first = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().written, 1);
    ASSERT_TRUE(first.value().next_segment);
    EXPECT_EQ(*first.value().next_segment, 7);
    EXPECT_EQ(queue_->snapshot().queued_records, 1);
    EXPECT_EQ(budget_->snapshot().records, 1);
    auto second = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().written, 1);
    EXPECT_EQ(segment_->metadata.snapshot().written, 2);
    close();
    EXPECT_TRUE(seal_to_commit(segment_).ok());
}

TEST_F(
    PipelineWriterTest,
    PartialUnwrittenFencesAdmissionAndSettlesUndispatchedQueueTailAsKnownFailed) {
    prepare(R"({"mode":"prefix_unwritten","prefix":1})");
    ASSERT_TRUE(writer_);
    for (int i = 0; i < 5; ++i) {
        submit(*first_);
    }
    auto first = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().written, 1);
    EXPECT_EQ(first.value().known_failed, 2);
    EXPECT_EQ(first.value().unknown, 0);
    EXPECT_TRUE(gate_->admission_fenced());
    EXPECT_EQ(budget_->snapshot().records, 2);
    auto tail = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(tail.ok());
    EXPECT_EQ(tail.value().written, 0);
    EXPECT_EQ(tail.value().known_failed, 2);
    EXPECT_EQ(segment_->metadata.snapshot().accepted, 5);
    EXPECT_EQ(segment_->metadata.snapshot().known_failed, 4);
    EXPECT_EQ(segment_->metadata.snapshot().unknown, 0);
    close();
    auto commit = seal_to_commit(segment_);
    ASSERT_TRUE(commit.ok());
    EXPECT_TRUE(commit.value()->commit(*ledger_).ok());
}

TEST_F(PipelineWriterTest, UnknownSuffixContaminatesOnlyDispatchedRecordsAndPreventsNormalSeal) {
    prepare(R"({"mode":"prefix_unknown","prefix":1})");
    ASSERT_TRUE(writer_);
    for (int i = 0; i < 5; ++i) {
        submit(*first_);
    }
    auto first = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().written, 1);
    EXPECT_EQ(first.value().unknown, 2);
    EXPECT_TRUE(gate_->admission_fenced());
    auto tail = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(tail.ok());
    EXPECT_EQ(tail.value().known_failed, 2);
    EXPECT_EQ(segment_->metadata.snapshot().unknown, 2);
    EXPECT_EQ(segment_->metadata.snapshot().known_failed, 2);
    EXPECT_EQ(budget_->snapshot().records, 0);
    close();
    EXPECT_FALSE(writer_->seal(segment_, soon()).ok());
    EXPECT_FALSE(ledger_->get(7).ok());
    EXPECT_TRUE(writer_->quiesce(soon()).ok());
    EXPECT_EQ(capacity_->snapshot().commit_slots, 1);
}

TEST_F(PipelineWriterTest, FailedLazyOpenNeverClassifiesAnUnenteredWriteAsUnknown) {
    prepare(R"({"mode":"fail_open"})");
    ASSERT_TRUE(writer_);
    submit(*first_);
    auto step = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(step.ok());
    EXPECT_EQ(step.value().known_failed, 1);
    EXPECT_EQ(step.value().unknown, 0);
    EXPECT_TRUE(gate_->admission_fenced());
    EXPECT_EQ(segment_->metadata.snapshot().native_inflight, 0);
    EXPECT_EQ(segment_->metadata.snapshot().known_failed, 1);
    EXPECT_EQ(budget_->snapshot().records, 0);
    EXPECT_FALSE(ledger_->get(7).ok());
}

TEST_F(PipelineWriterTest, InvalidNativeBatchOutputFencesAndMakesTheEntireDispatchedBatchUnknown) {
    prepare(R"({"mode":"invalid_output"})");
    ASSERT_TRUE(writer_);
    submit(*first_);
    submit(*second_);
    auto step = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(step.ok());
    EXPECT_EQ(step.value().failure, StatusCode::Internal);
    EXPECT_EQ(step.value().written, 0);
    EXPECT_EQ(step.value().unknown, 2);
    EXPECT_TRUE(gate_->admission_fenced());
    EXPECT_EQ(segment_->metadata.snapshot().unknown, 2);
    close();
    EXPECT_FALSE(writer_->seal(segment_, soon()).ok());
}

TEST_F(PipelineWriterTest, FullConfirmedPrefixOnNonOkRemainsWrittenAndCanBeSealed) {
    prepare(R"({"mode":"full_failed"})");
    ASSERT_TRUE(writer_);
    submit(*first_);
    submit(*first_);
    auto step = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(step.ok());
    EXPECT_EQ(step.value().written, 2);
    EXPECT_EQ(step.value().known_failed, 0);
    EXPECT_EQ(step.value().unknown, 0);
    EXPECT_TRUE(gate_->admission_fenced());
    close();
    EXPECT_TRUE(seal_to_commit(segment_).ok());
}

TEST_F(PipelineWriterTest, LateNativeReturnKeepsPayloadAndCapacityAfterControlClosesGate) {
    prepare(R"({"mode":"late_write","delay_ms":100})");
    ASSERT_TRUE(writer_);
    submit(*first_);
    auto work = std::async(std::launch::async, [&] {
        return writer_->write_available(*queue_, segment_, plugin::Deadline::after(1ms).value());
    });
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (segment_->metadata.snapshot().native_inflight == 0 &&
           std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(work.wait_for(5ms), std::future_status::timeout);
    close();
    EXPECT_EQ(segment_->metadata.snapshot().native_inflight, 1);
    EXPECT_EQ(budget_->snapshot().records, 1);
    EXPECT_EQ(pool_->snapshot().live_buffers, 1);
    EXPECT_EQ(capacity_->snapshot().commit_slots, 1);
    auto actual = work.get();
    ASSERT_TRUE(actual.ok());
    EXPECT_EQ(actual.value().written, 1);
    EXPECT_EQ(segment_->metadata.snapshot().native_inflight, 0);
    EXPECT_EQ(budget_->snapshot().records, 0);
    EXPECT_TRUE(seal_to_commit(segment_).ok());
}

TEST_F(PipelineWriterTest, SegmentCutKeepsOldAndNewAcceptedRecordsBoundToTheirOwnReservations) {
    prepare();
    ASSERT_TRUE(writer_);
    submit(*first_);
    ASSERT_TRUE(writer_->write_available(*queue_, segment_, soon()).ok());
    auto next = new_segment(8);
    ASSERT_TRUE(next.ok());
    ASSERT_TRUE(
        writer_->prepare_descriptor(next.value(), first_->active_descriptor, 200, soon()).ok());
    ASSERT_TRUE(
        writer_->prepare_descriptor(next.value(), second_->active_descriptor, 200, soon()).ok());
    submit(*first_);
    {
        auto cut = gate_->enter_segment_cut(id_, 7);
        ASSERT_TRUE(cut.ok());
        segment_->close_admission();
        std::lock_guard<std::mutex> first(first_->mutex);
        std::lock_guard<std::mutex> second(second_->mutex);
        first_->segment = next.value();
        second_->segment = next.value();
    }
    submit(*first_);
    auto old = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(old.ok());
    EXPECT_EQ(old.value().written, 1);
    EXPECT_EQ(old.value().next_segment, 8);
    EXPECT_EQ(queue_->snapshot().queued_records, 1);
    auto old_commit = seal_to_commit(segment_);
    ASSERT_TRUE(old_commit.ok());
    auto fresh = writer_->write_available(*queue_, next.value(), soon());
    ASSERT_TRUE(fresh.ok());
    EXPECT_EQ(fresh.value().written, 1);
    ASSERT_TRUE(gate_->close_and_capture(id_).ok());
    next.value()->close_admission();
    auto new_commit = seal_to_commit(next.value());
    ASSERT_TRUE(new_commit.ok());
    EXPECT_TRUE(old_commit.value()->commit(*ledger_).ok());
    EXPECT_TRUE(new_commit.value()->commit(*ledger_).ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committed);
    EXPECT_EQ(ledger_->get(8).value().state, artifact::LedgerState::Committed);
}

TEST_F(PipelineWriterTest, InitialDescriptorFailureCannotDeclareReadyOrOpenAdmission) {
    prepare(R"({"mode":"fail_descriptor"})", false);
    ASSERT_TRUE(writer_);
    EXPECT_FALSE(
        writer_->prepare_descriptor(segment_, first_->active_descriptor, 100, soon()).ok());
    EXPECT_TRUE(gate_->admission_fenced());
    EXPECT_FALSE(gate_->snapshot().open);
    EXPECT_EQ(segment_->metadata.snapshot().pending_descriptors, 0);
    EXPECT_EQ(segment_->metadata.snapshot().accepted, 0);
    EXPECT_EQ(gate_->open_admission(id_, 7), pipeline::GateOpenResult::Obsolete);
    EXPECT_FALSE(fs::exists(volume_->artifacts_directory() / "7.tmp"));
}

TEST_F(PipelineWriterTest, SealFailureKeepsWrittenEvidenceAndAllCommitCapacityWithoutRetry) {
    prepare(R"({"mode":"fail_seal"})");
    ASSERT_TRUE(writer_);
    submit(*first_);
    ASSERT_TRUE(writer_->write_available(*queue_, segment_, soon()).ok());
    close();
    EXPECT_FALSE(writer_->seal(segment_, soon()).ok());
    EXPECT_EQ(writer_->seal(segment_, soon()).status().code(), StatusCode::InvalidState);
    EXPECT_EQ(segment_->metadata.snapshot().written, 1);
    EXPECT_EQ(segment_->metadata.snapshot().unknown, 0);
    EXPECT_EQ(capacity_->snapshot().commit_slots, 1);
    EXPECT_FALSE(ledger_->get(7).ok());
    EXPECT_TRUE(writer_->quiesce(soon()).ok());
}

TEST_F(
    PipelineWriterTest, PreparedCatalogLimitRejectsOnlyTheNewDescriptorBeforeNativeAndAdmission) {
    prepare("{}", true, 2);
    ASSERT_TRUE(writer_);
    const auto before = capacity_->snapshot().future_bytes;
    auto third = descriptor(1, 2);
    EXPECT_EQ(
        writer_->prepare_descriptor(segment_, third, 100, soon()).status().code(),
        StatusCode::ResourceExhausted);
    EXPECT_FALSE(gate_->admission_fenced());
    EXPECT_EQ(capacity_->snapshot().future_bytes, before);
    EXPECT_EQ(segment_->metadata.snapshot().descriptors, 2);
    submit(*first_);
    auto step = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(step.ok());
    EXPECT_EQ(step.value().written, 1);
    close();
    EXPECT_TRUE(seal_to_commit(segment_).ok());
}

TEST_F(PipelineWriterTest, ControlFenceRejectsLaterDescriptorWorkWhileAcceptedRecordsStillDrain) {
    prepare();
    ASSERT_TRUE(writer_);
    submit(*first_);
    ASSERT_TRUE(gate_->establish_control_fence(id_).ok());
    auto proposed = descriptor(1, 2);
    EXPECT_EQ(
        writer_->prepare_descriptor(segment_, proposed, 200, soon()).status().code(),
        StatusCode::InvalidState);
    EXPECT_EQ(segment_->metadata.snapshot().descriptors, 2);
    auto drained = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(drained.ok());
    EXPECT_EQ(drained.value().written, 1);
    EXPECT_EQ(drained.value().failure, StatusCode::Ok);
    EXPECT_TRUE(gate_->admission_fenced());
    close();
    EXPECT_TRUE(seal_to_commit(segment_).ok());
}

TEST_F(PipelineWriterTest, KnownVolumeIdentityFailureStopsNewNativeWritesWithoutInventingUnknown) {
    prepare();
    ASSERT_TRUE(writer_);
    submit(*first_);
    ASSERT_TRUE(writer_->write_available(*queue_, segment_, soon()).ok());
    submit(*first_);
    const auto path = volume_->artifacts_directory() / "7.tmp" / "records.fake";
    const auto before = fs::file_size(path);
    std::ofstream(volume_->root() / ".rodaq" / "volume.json", std::ios::trunc) << "{}";
    ASSERT_FALSE(volume_->verify().ok());
    auto proposed = descriptor(1, 2);
    EXPECT_FALSE(writer_->prepare_descriptor(segment_, proposed, 200, soon()).ok());
    EXPECT_EQ(segment_->metadata.snapshot().descriptors, 2);
    auto rejected = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(rejected.ok());
    EXPECT_NE(rejected.value().failure, StatusCode::Ok);
    EXPECT_EQ(rejected.value().written, 0);
    EXPECT_EQ(rejected.value().known_failed, 1);
    EXPECT_EQ(rejected.value().unknown, 0);
    EXPECT_EQ(fs::file_size(path), before);
    EXPECT_TRUE(gate_->admission_fenced());
    EXPECT_EQ(segment_->metadata.snapshot().written, 1);
    EXPECT_EQ(segment_->metadata.snapshot().known_failed, 1);
    EXPECT_FALSE(ledger_->get(7).ok());
}

}  // namespace
