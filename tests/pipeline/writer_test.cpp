#include "pipeline/pipeline_writer.hpp"
#include "artifact/segment_committer.hpp"
#include "pipeline/admission.hpp"
#include "pipeline/descriptor.hpp"
#include "pipeline/stream_table.hpp"
#include "../artifact/manifest_fixture.hpp"

#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;
namespace pipeline = rodaq::pipeline;
namespace artifact = rodaq::artifact;
namespace plugin = rodaq::plugin;
using rodaq::StatusCode;
using namespace std::chrono_literals;

plugin::Deadline soon() { return plugin::Deadline::after(2s).value(); }

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
        fs::create_directory(root_ / "volume");
        auto managed =
            artifact::ManagedVolume::open(root_ / "volume", artifact::VolumeOpenMode::Initialize);
        ASSERT_TRUE(managed.ok());
        volume_ = std::move(managed.value());
        auto capacity = artifact::VolumeCapacity::create({32000000, 8000000, 100, 10000, 8, 8, 16});
        ASSERT_TRUE(capacity.ok());
        capacity_.emplace(std::move(capacity.value()));
        auto probe = capacity_->begin_space_sample(100);
        ASSERT_TRUE(probe.ok());
        ASSERT_TRUE(capacity_->finish_space_sample(probe.value(), 32000000, 100).ok());

        id_ = pipeline::parse_instance_id("21234567-89ab-4cde-8fab-0123456789ab").value();
        gate_.emplace(id_, 7);
        auto streams = pipeline::StreamTable::create({{1, "/first", 8}, {2, "/second", 8}});
        ASSERT_TRUE(streams.ok());
        streams_.emplace(std::move(streams.value()));
        first_ = streams_->find(1);
        second_ = streams_->find(2);
        queue_.emplace(std::move(pipeline::RecordQueue::create(16).value()));
        budget_.emplace(std::move(pipeline::RecordBudget::create(32, 256).value()));
        pool_.emplace(std::move(pipeline::PayloadPool::create({16384, 256, 8, 32}).value()));
        const auto task = rodaq::test::manifest_identity().task_id;
        domain_.emplace(pipeline::AdmissionDomain{
            task, 3, id_, 7, *gate_, *budget_, *pool_, *queue_, {100, 1000}, 0, 200});
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
        volume_.reset();
        std::error_code error;
        fs::remove_all(root_, error);
    }

    rodaq::storage::PrepareConfig envelope() const {
        return {{sizeof(rodaq::storage::PrepareConfig)}, 8, 12, 3, 4, 100, 800, 1000000, 1, 32};
    }

    rodaq::Result<std::shared_ptr<pipeline::RecordingSegment>> new_segment(std::uint64_t id) {
        auto identity = rodaq::test::manifest_identity(id);
        identity.volume_id = volume_->identity();
        identity.storage = library_->info();
        identity.playback_anchor = {100, 1000};
        artifact::ManifestLimits limits;
        limits.max_bytes = 16384;
        limits.max_files = 1;
        limits.max_streams = 2;
        limits.max_descriptors = 4;
        return pipeline::RecordingSegment::prepare(
            std::move(identity), {1, 2}, envelope(), limits, *capacity_, 65536, 100);
    }

    pipeline::DescriptorRef make_descriptor(rodaq::StreamId stream, std::uint64_t id) {
        rodaq::acquisition::Descriptor proposal{};
        proposal.header.struct_size = sizeof(proposal);
        proposal.stream_id = stream;
        proposal.data_profile = RODAQ_DATA_PROFILE_GENERIC;
        proposal.channel_name = {"/fake", 5};
        proposal.message_encoding = {"json", 4};
        auto result = pipeline::Descriptor::create(proposal, id);
        EXPECT_TRUE(result.ok());
        return result.ok() ? std::move(result.value()) : nullptr;
    }

    void prepare(std::string_view config = "{}", std::uint32_t catalog_capacity = 8) {
        auto instance = library_->create_instance(config);
        ASSERT_TRUE(instance.ok()) << instance.status().message();
        instance_.emplace(std::move(instance.value()));
        auto handle = instance_->query<rodaq::storage::Interface>();
        ASSERT_TRUE(handle.ok());
        auto writer = pipeline::PipelineWriter::prepare(
            std::move(handle.value()), envelope(), volume_, *gate_, id_, catalog_capacity, soon());
        ASSERT_TRUE(writer.ok()) << writer.status().message();
        writer_ = std::move(writer.value());
        auto segment = new_segment(7);
        ASSERT_TRUE(segment.ok());
        segment_ = std::move(segment.value());
        first_->active_descriptor = make_descriptor(1, 1);
        second_->active_descriptor = make_descriptor(2, 2);
        first_->segment = segment_;
        second_->segment = segment_;
        ASSERT_TRUE(writer_->prepare_descriptor(segment_, first_->active_descriptor, soon()).ok());
        ASSERT_TRUE(writer_->prepare_descriptor(segment_, second_->active_descriptor, soon()).ok());
        ASSERT_EQ(gate_->open_admission(id_, 7), pipeline::GateOpenResult::Opened);
    }

    void submit(pipeline::StreamAdmission& stream, std::size_t bytes = 4) {
        std::array<std::uint8_t, 8> payload{1, 2, 3, 4, 5, 6, 7, 8};
        rodaq::acquisition::RecordAttempt attempt{};
        attempt.header.struct_size = sizeof(attempt);
        attempt.stream_id = stream.stream_id;
        attempt.descriptor_id = stream.active_descriptor->descriptor_id;
        attempt.payload = {payload.data(), static_cast<std::uint32_t>(bytes)};
        const rodaq::time::ClockSample received{200, 1100, 1};
        const auto accepted = pipeline::submit_record(*domain_, stream, attempt, received);
        ASSERT_TRUE(accepted.ok());
        ASSERT_EQ(accepted.value(), RODAQ_SUBMIT_ACCEPTED);
        payload.fill(9);
    }

    void close_segment() {
        ASSERT_TRUE(gate_->close_and_capture(id_).ok());
        segment_->close_admission();
    }

    fs::path root_;
    std::optional<plugin::PluginManager> manager_;
    std::optional<plugin::PluginLibrary> library_;
    std::optional<plugin::PluginInstance> instance_;
    std::shared_ptr<artifact::ManagedVolume> volume_;
    std::optional<artifact::VolumeCapacity> capacity_;
    pipeline::InstanceId id_;
    std::optional<pipeline::AdmissionGate> gate_;
    std::optional<pipeline::StreamTable> streams_;
    pipeline::StreamAdmission* first_{nullptr};
    pipeline::StreamAdmission* second_{nullptr};
    std::optional<pipeline::RecordQueue> queue_;
    std::optional<pipeline::RecordBudget> budget_;
    std::optional<pipeline::PayloadPool> pool_;
    std::optional<pipeline::AdmissionDomain> domain_;
    std::unique_ptr<pipeline::PipelineWriter> writer_;
    std::shared_ptr<pipeline::RecordingSegment> segment_;
};

TEST_F(PipelineWriterTest, WritesDrainsSealsAndCommitsCleanSegment) {
    prepare();
    submit(*first_, 4);
    submit(*second_, 4);
    auto step = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(step.ok()) << step.status().message();
    EXPECT_EQ(step.value().failure, StatusCode::Ok);
    EXPECT_EQ(step.value().written, 2U);
    EXPECT_EQ(step.value().known_failed, 0U);
    EXPECT_EQ(step.value().unknown, 0U);
    EXPECT_EQ(budget_->snapshot().records, 0U);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0U);

    close_segment();
    auto sealed = writer_->seal(segment_, soon());
    ASSERT_TRUE(sealed.ok()) << sealed.status().message();
    ASSERT_EQ(sealed.value().files.size(), 1U);

    auto files = artifact::SealedFiles::open(
        volume_, segment_->metadata.segment_id(), sealed.value(), envelope(), 16384);
    ASSERT_TRUE(files.ok());
    auto manifest = segment_->metadata.capture_manifest(
        files.value()->evidence(),
        rodaq::WallTime{std::chrono::nanoseconds{1700000000000000000LL}});
    ASSERT_TRUE(manifest.ok());
    EXPECT_EQ(manifest.value().coverage.size(), 2U);
    EXPECT_EQ(manifest.value().logical_bytes, 8ULL);
    auto reservation = segment_->handoff_sealed(files.value()->storage_bytes());
    ASSERT_TRUE(reservation.ok());
    auto committer = artifact::SegmentCommitter::create(
        files.value(), manifest.value(), {}, reservation.value());
    ASSERT_TRUE(committer.ok());
    ASSERT_TRUE(committer.value()->commit().ok());
    EXPECT_TRUE(fs::is_directory(volume_->artifacts_directory() / "7"));
}

TEST_F(PipelineWriterTest, PartialUnknownWriteClassifiesConfirmedPrefixAndBlocksNormalCommit) {
    prepare(R"({"mode":"prefix_unknown","prefix":1})");
    submit(*first_, 4);
    submit(*first_, 4);
    submit(*first_, 4);
    auto step = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(step.ok());
    EXPECT_NE(step.value().failure, StatusCode::Ok);
    EXPECT_EQ(step.value().written, 1U);
    EXPECT_EQ(step.value().unknown, 2U);

    close_segment();
    // Storage with Unknown tail refuses normal seal_segment (fail-closed).
    EXPECT_FALSE(writer_->seal(segment_, soon()).ok());
    const auto snap = segment_->metadata.snapshot();
    EXPECT_EQ(snap.written, 1ULL);
    EXPECT_EQ(snap.unknown, 2ULL);
}

TEST_F(PipelineWriterTest, PartialDefinitelyUnwrittenPreservesConfirmedPrefixAndCommits) {
    prepare(R"({"mode":"prefix_unwritten","prefix":1})");
    submit(*first_, 4);
    submit(*first_, 4);
    auto step = writer_->write_available(*queue_, segment_, soon());
    ASSERT_TRUE(step.ok());
    EXPECT_NE(step.value().failure, StatusCode::Ok);
    EXPECT_EQ(step.value().written, 1U);
    EXPECT_EQ(step.value().known_failed, 1U);
    EXPECT_EQ(step.value().unknown, 0U);
}

}  // namespace
