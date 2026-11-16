#include "artifact/manifest.hpp"
#include "persistence/digest.hpp"
#include "persistence/json.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string>

namespace {

namespace artifact = rodaq::artifact;
namespace persistence = rodaq::persistence;
using rodaq::StatusCode;

artifact::SegmentManifest manifest() {
    artifact::SegmentManifest result;
    result.identity.segment_id = 7;
    result.identity.task_id = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
    result.identity.pipeline_id = 3;
    result.identity.instance_id = "21234567-89ab-4cde-8fab-0123456789ab";
    result.identity.segment_sequence = 1;
    result.identity.config_version = "sha256:" + std::string(64, 'c');
    result.identity.effective_config_id = "sha256:" + std::string(64, 'd');
    result.identity.control_generation = 7;
    result.identity.reset_generation_at_commit = 2;
    result.identity.volume_id =
        artifact::parse_volume_id("11234567-89ab-4cde-8fab-0123456789ab").value();
    result.identity.acquisition = {"rodaq.fake.acquisition", "1.0.0", std::string(64, 'e')};
    result.identity.storage = {"rodaq.fake.storage", "1.0.0", std::string(64, 'f')};
    result.identity.qualified_runtime_profile_id = "behavioral-fake.v1";
    result.identity.format = "fake";
    result.logical_bytes = 3;
    result.files = {
        {"data.bin", 3, "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}};
    artifact::ManifestDescriptor descriptor;
    descriptor.stream_id = 1;
    descriptor.descriptor_id = 1;
    descriptor.accepted_records = 2;
    descriptor.data_profile = RODAQ_DATA_PROFILE_GENERIC;
    descriptor.channel_name = "/data";
    descriptor.message_encoding = "json";
    descriptor.schema_digest =
        "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    result.descriptors.push_back(std::move(descriptor));
    result.coverage = {{1, 2, 1, 1, 0, 10, 11}};
    result.identity.playback_anchor = {42, 10, 1000};
    result.epochs = {{1, rodaq::time::ClockEpochOrigin::Boot, {}, 2, 100, 200, 1}};
    result.first_receive_monotonic = 100;
    result.last_receive_monotonic = 200;
    result.min_receive_realtime = 1100;
    result.max_receive_realtime = 1200;
    result.recording_end_ns = 100;
    return result;
}

TEST(SegmentManifest, IndependentGoldenBindsIdentityFilesDescriptorsCoverageAndPlaybackFacts) {
    const auto value = manifest();
    auto encoded = artifact::encode_manifest(value, {});
    ASSERT_TRUE(encoded.ok());
    auto decoded = artifact::decode_manifest(encoded.value(), {});
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    auto digest = artifact::manifest_digest(value, {});
    ASSERT_TRUE(digest.ok());
    // Independent Python hashlib + sorted compact JSON, using the same
    // explicit ASCII fixture facts, not encoder output as golden input.
    EXPECT_EQ(
        digest.value(), "sha256:d8cb67c34f1f98282644806b22bc665142951f4a22a40717a09a3fa81dd27901");
    auto document = persistence::parse_json(encoded.value());
    ASSERT_TRUE(document.ok());
    EXPECT_EQ(document.value().at("content_digest"), digest.value());
    EXPECT_EQ(decoded.value().identity.acquisition.sha256, value.identity.acquisition.sha256);
    EXPECT_EQ(decoded.value().coverage[0].known_failed, 1);
    EXPECT_EQ(decoded.value().identity.playback_anchor.realtime_ns, 1000);
    EXPECT_EQ(decoded.value().descriptors[0].schema_digest, value.descriptors[0].schema_digest);
}

TEST(SegmentManifest, FullWidthNumbersAndClockDiscontinuityFactsRemainExact) {
    auto value = manifest();
    value.identity.segment_id = std::numeric_limits<std::uint64_t>::max();
    value.recording_end_ns = std::numeric_limits<std::uint64_t>::max();
    value.logical_bytes = std::numeric_limits<std::uint64_t>::max();
    value.identity.playback_anchor.realtime_ns = std::numeric_limits<std::int64_t>::min();
    value.min_receive_realtime = std::numeric_limits<std::int64_t>::min();
    value.max_receive_realtime = std::numeric_limits<std::int64_t>::max();
    value.epochs[0].accepted_records = 1;
    value.epochs[0].last_receive_monotonic = 100;
    value.epochs.push_back(
        {2, rodaq::time::ClockEpochOrigin::RealtimeJump, rodaq::time::ClockJump{1, -500, 150}, 1,
         200, 200, 2});
    auto encoded = artifact::encode_manifest(value, {});
    ASSERT_TRUE(encoded.ok());
    auto decoded = artifact::decode_manifest(encoded.value(), {});
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().logical_bytes, value.logical_bytes);
    EXPECT_EQ(decoded.value().identity.segment_id, value.identity.segment_id);
    EXPECT_EQ(decoded.value().recording_end_ns, value.recording_end_ns);
    EXPECT_EQ(
        decoded.value().identity.playback_anchor.realtime_ns,
        value.identity.playback_anchor.realtime_ns);
    EXPECT_EQ(decoded.value().max_receive_realtime, value.max_receive_realtime);
    EXPECT_EQ(decoded.value().epochs[1].jump->delta_ns, -500);
}

TEST(SegmentManifest, CoverageDescriptorAndEpochCountsMustDescribeExactlyTheSameAcceptedSet) {
    auto value = manifest();
    ++value.descriptors[0].accepted_records;
    EXPECT_EQ(artifact::validate_manifest(value, {}).status().code(), StatusCode::InvalidArgument);
    value = manifest();
    ++value.epochs[0].accepted_records;
    EXPECT_EQ(artifact::validate_manifest(value, {}).status().code(), StatusCode::InvalidArgument);
    value = manifest();
    ++value.coverage[0].written;
    EXPECT_EQ(artifact::validate_manifest(value, {}).status().code(), StatusCode::InvalidArgument);
    value = manifest();
    value.descriptors.push_back(value.descriptors[0]);
    EXPECT_EQ(artifact::validate_manifest(value, {}).status().code(), StatusCode::InvalidArgument);
    value = manifest();
    value.epochs[0].first_receive_monotonic = 99;
    EXPECT_EQ(artifact::validate_manifest(value, {}).status().code(), StatusCode::InvalidArgument);
}

TEST(
    SegmentManifest, UnknownSettlementIsPreservedAsEvidenceAndDoesNotImplyNormalCommitEligibility) {
    auto value = manifest();
    value.coverage[0].known_failed = 0;
    value.coverage[0].unknown = 1;
    auto encoded = artifact::encode_manifest(value, {});
    ASSERT_TRUE(encoded.ok());
    auto decoded = artifact::decode_manifest(encoded.value(), {});
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().coverage[0].unknown, 1);
    // Committer, not this codec, must reject normal publication of Unknown.
}

TEST(SegmentManifest, IntegrityClosedSchemaAndNumericStringsRejectMalformedPersistentInput) {
    auto encoded = artifact::encode_manifest(manifest(), {});
    ASSERT_TRUE(encoded.ok());
    auto document = persistence::parse_json(encoded.value()).value();
    document["playback_anchor_realtime"] = "999";
    EXPECT_EQ(artifact::decode_manifest(document.dump(), {}).status().code(), StatusCode::DataLoss);
    document = persistence::parse_json(encoded.value()).value();
    document["coverage"][0]["accepted"] = 2;
    auto altered = persistence::encode_integrity(document, "rodaq.segment-manifest.v1");
    ASSERT_TRUE(altered.ok());
    EXPECT_EQ(artifact::decode_manifest(altered.value(), {}).status().code(), StatusCode::DataLoss);
    document = persistence::parse_json(encoded.value()).value();
    document["extra"] = nullptr;
    altered = persistence::encode_integrity(document, "rodaq.segment-manifest.v1");
    ASSERT_TRUE(altered.ok());
    EXPECT_EQ(artifact::decode_manifest(altered.value(), {}).status().code(), StatusCode::DataLoss);
    document = persistence::parse_json(encoded.value()).value();
    document["format_version"] = "2.0";
    altered = persistence::encode_integrity(document, "rodaq.segment-manifest.v1");
    ASSERT_TRUE(altered.ok());
    EXPECT_EQ(
        artifact::decode_manifest(altered.value(), {}).status().code(), StatusCode::Unavailable);
}

TEST(SegmentManifest, CaptureOwnsBorrowedTextAndComputesSchemaDigestBeforeInputBackingChanges) {
    std::array<char, 5> channel{'/', 'd', 'a', 't', 'a'};
    std::array<std::uint8_t, 3> schema{0, 1, 255};
    rodaq::storage::StreamDescriptor descriptor{};
    descriptor.header.struct_size = sizeof(descriptor);
    descriptor.stream_id = 1;
    descriptor.descriptor_id = 1;
    descriptor.data_profile = RODAQ_DATA_PROFILE_GENERIC;
    descriptor.channel_name = {channel.data(), channel.size()};
    descriptor.message_encoding = {"json", 4};
    descriptor.schema = {schema.data(), schema.size()};
    const auto expected = persistence::byte_digest(schema.data(), schema.size());
    ASSERT_TRUE(expected.ok());
    auto captured = artifact::capture_manifest_descriptor(descriptor, 2);
    ASSERT_TRUE(captured.ok());
    channel.fill('x');
    schema.fill(0);
    EXPECT_EQ(captured.value().channel_name, "/data");
    EXPECT_EQ(captured.value().schema_digest, expected.value());
    EXPECT_EQ(captured.value().schema_size, 3);
    descriptor.schema.size = std::numeric_limits<std::uint32_t>::max();
    EXPECT_EQ(
        artifact::capture_manifest_descriptor(descriptor, 2).status().code(),
        StatusCode::InvalidArgument);
}

TEST(SegmentManifest, Ros2QosUsesNormalizedTypedSetAndPreservesExactDepthAndDurationFields) {
    const std::string schema = "uint32 value\n";
    std::array<rodaq_ros2_qos_profile_t, 3> qos{};
    qos[0].reliability = RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT;
    qos[1].reliability = RODAQ_ROS2_QOS_RELIABILITY_RELIABLE;
    qos[1].depth = std::numeric_limits<std::uint64_t>::max();
    qos[1].deadline = {9223372036, 854775807};
    qos[2] = qos[0];
    rodaq::storage::StreamDescriptor descriptor{};
    descriptor.header.struct_size = sizeof(descriptor);
    descriptor.stream_id = 1;
    descriptor.descriptor_id = 1;
    descriptor.data_profile = RODAQ_DATA_PROFILE_ROS2;
    descriptor.channel_name = {"/data", 5};
    descriptor.message_encoding = {"cdr", 3};
    descriptor.schema_name = {"demo/msg/Value", 14};
    descriptor.schema_encoding = {"ros2msg", 7};
    descriptor.schema = {
        reinterpret_cast<const std::uint8_t*>(schema.data()),
        static_cast<std::uint32_t>(schema.size())};
    descriptor.ros2_offered_qos_profiles = qos.data();
    descriptor.ros2_offered_qos_profile_count = qos.size();
    auto captured = artifact::capture_manifest_descriptor(descriptor, 2);
    ASSERT_TRUE(captured.ok());
    ASSERT_EQ(captured.value().offered_qos.size(), 2);
    auto value = manifest();
    value.descriptors[0] = std::move(captured.value());
    auto encoded = artifact::encode_manifest(value, {});
    ASSERT_TRUE(encoded.ok());
    auto decoded = artifact::decode_manifest(encoded.value(), {});
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(
        decoded.value().descriptors[0].offered_qos[0].depth,
        std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(decoded.value().descriptors[0].offered_qos[0].deadline.nsec, 854775807);
}

TEST(SegmentManifest, ResourceLimitsAndReservedPathsApplyBeforePersistence) {
    auto value = manifest();
    value.files[0].relative_path = "manifest.json";
    EXPECT_FALSE(artifact::encode_manifest(value, {}).ok());
    value = manifest();
    artifact::ManifestLimits limits;
    limits.max_bytes = 128;
    EXPECT_FALSE(artifact::encode_manifest(value, limits).ok());
    value.epochs.push_back(value.epochs[0]);
    limits = {};
    limits.max_epochs = 1;
    EXPECT_FALSE(artifact::encode_manifest(value, limits).ok());
}

}  // namespace
