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

artifact::SegmentManifest make_manifest() {
    artifact::SegmentManifest result;
    result.identity.segment_id = 7;
    result.identity.task_id = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
    result.identity.pipeline_id = 3;
    result.identity.instance_id = "21234567-89ab-4cde-8fab-0123456789ab";
    result.identity.segment_sequence = 1;
    result.identity.committed_at = rodaq::WallTime{std::chrono::nanoseconds{1700000000000000000LL}};
    result.identity.config_version = "sha256:" + std::string(64, 'c');
    result.identity.deployment_generation = "1";
    result.identity.control_generation = 7;
    result.identity.volume_id =
        artifact::parse_volume_id("11234567-89ab-4cde-8fab-0123456789ab").value();
    result.identity.acquisition = {"rodaq.fake.acquisition", "Fake Acquisition", "1.0.0"};
    result.identity.storage = {"rodaq.fake.storage", "Fake Storage", "1.0.0"};
    result.identity.qualified_runtime_profile_id = "behavioral-fake.v1";
    result.identity.format = "fake";
    result.identity.playback_anchor = {10, 1000};
    result.logical_bytes = 3;
    result.files = {{"data.mcap", 3}};
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
    result.first_receive_monotonic = 100;
    result.last_receive_monotonic = 200;
    result.min_receive_realtime = 1100;
    result.max_receive_realtime = 1200;
    result.recording_begin_ns = 1090;
    result.recording_end_ns = 1190;
    return result;
}

TEST(SegmentManifest, RoundTripPreservesIdentityFilesDescriptorsCoverageAndPlaybackAnchor) {
    const auto value = make_manifest();
    auto encoded = artifact::encode_manifest(value, {});
    ASSERT_TRUE(encoded.ok()) << encoded.status().message();
    auto decoded = artifact::decode_manifest(encoded.value(), {});
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().identity.segment_id, 7ULL);
    EXPECT_EQ(decoded.value().identity.acquisition.plugin_id, "rodaq.fake.acquisition");
    EXPECT_EQ(decoded.value().identity.storage.plugin_version, "1.0.0");
    EXPECT_EQ(decoded.value().coverage[0].known_failed, 1ULL);
    EXPECT_EQ(decoded.value().identity.playback_anchor.monotonic_ns, 10ULL);
    EXPECT_EQ(decoded.value().identity.playback_anchor.realtime_ns, 1000LL);
    EXPECT_EQ(decoded.value().descriptors[0].schema_digest, value.descriptors[0].schema_digest);
    EXPECT_EQ(decoded.value().files.size(), 1U);
    EXPECT_EQ(decoded.value().files[0].relative_path, "data.mcap");
    EXPECT_EQ(decoded.value().files[0].physical_size, 3ULL);
}

TEST(SegmentManifest, FullWidth64BitFieldsRoundTripExactly) {
    auto value = make_manifest();
    value.identity.segment_id = std::numeric_limits<std::uint64_t>::max();
    value.recording_begin_ns = 100;
    value.recording_end_ns = std::numeric_limits<std::uint64_t>::max();
    value.logical_bytes = std::numeric_limits<std::uint64_t>::max();
    value.identity.playback_anchor.realtime_ns = std::numeric_limits<std::int64_t>::min();
    value.min_receive_realtime = std::numeric_limits<std::int64_t>::min();
    value.max_receive_realtime = std::numeric_limits<std::int64_t>::max();
    auto encoded = artifact::encode_manifest(value, {});
    ASSERT_TRUE(encoded.ok()) << encoded.status().message();
    auto decoded = artifact::decode_manifest(encoded.value(), {});
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().logical_bytes, value.logical_bytes);
    EXPECT_EQ(decoded.value().identity.segment_id, value.identity.segment_id);
    EXPECT_EQ(decoded.value().recording_end_ns, value.recording_end_ns);
    EXPECT_EQ(
        decoded.value().identity.playback_anchor.realtime_ns,
        value.identity.playback_anchor.realtime_ns);
    EXPECT_EQ(decoded.value().max_receive_realtime, value.max_receive_realtime);
}

TEST(SegmentManifest, CoverageAndDescriptorCountsMustDescribeExactlyTheSameAcceptedSet) {
    auto value = make_manifest();
    ++value.descriptors[0].accepted_records;
    EXPECT_EQ(artifact::validate_manifest(value, {}).status().code(), StatusCode::InvalidArgument);
    value = make_manifest();
    ++value.coverage[0].written;
    EXPECT_EQ(artifact::validate_manifest(value, {}).status().code(), StatusCode::InvalidArgument);
    value = make_manifest();
    value.descriptors.push_back(value.descriptors[0]);
    EXPECT_EQ(artifact::validate_manifest(value, {}).status().code(), StatusCode::InvalidArgument);
}

TEST(SegmentManifest, UnknownSettlementIsPreservedAsEvidence) {
    auto value = make_manifest();
    value.coverage[0].known_failed = 0;
    value.coverage[0].unknown = 1;
    auto encoded = artifact::encode_manifest(value, {});
    ASSERT_TRUE(encoded.ok());
    auto decoded = artifact::decode_manifest(encoded.value(), {});
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().coverage[0].unknown, 1ULL);
}

TEST(SegmentManifest, ClosedSchemaRejectsUnknownFieldsAndUnsupportedVersions) {
    auto encoded = artifact::encode_manifest(make_manifest(), {});
    ASSERT_TRUE(encoded.ok());
    auto document = persistence::parse_json(encoded.value()).value();
    document["extra"] = nullptr;
    EXPECT_EQ(artifact::decode_manifest(document.dump(), {}).status().code(), StatusCode::DataLoss);
    document = persistence::parse_json(encoded.value()).value();
    document["format_version"] = "2.0";
    EXPECT_EQ(
        artifact::decode_manifest(document.dump(), {}).status().code(), StatusCode::Unavailable);
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
    EXPECT_EQ(captured.value().schema_size, 3U);
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
    ASSERT_EQ(captured.value().offered_qos.size(), 2U);
    auto value = make_manifest();
    value.descriptors[0] = std::move(captured.value());
    value.descriptors[0].ros2_subscription = rodaq::canonical::Ros2SubscriptionFacts{
        "humble", "rmw_fastrtps_cpp", qos[1], std::numeric_limits<std::uint64_t>::max(), 1, 2};
    auto encoded = artifact::encode_manifest(value, {});
    ASSERT_TRUE(encoded.ok()) << encoded.status().message();
    auto decoded = artifact::decode_manifest(encoded.value(), {});
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(
        decoded.value().descriptors[0].offered_qos[0].depth,
        std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(decoded.value().descriptors[0].offered_qos[0].deadline.nsec, 854775807);
    ASSERT_TRUE(decoded.value().descriptors[0].ros2_subscription);
    EXPECT_EQ(
        decoded.value().descriptors[0].ros2_subscription->subscription_generation,
        std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(
        decoded.value().descriptors[0].ros2_subscription->installed_requested_qos.depth,
        std::numeric_limits<std::uint64_t>::max());
}

TEST(SegmentManifest, ResourceLimitsAndReservedPathsApplyBeforePersistence) {
    auto value = make_manifest();
    value.files[0].relative_path = "manifest.json";
    EXPECT_FALSE(artifact::encode_manifest(value, {}).ok());
    value = make_manifest();
    artifact::ManifestLimits limits;
    limits.max_bytes = 128;
    EXPECT_FALSE(artifact::encode_manifest(value, limits).ok());
}

}  // namespace
