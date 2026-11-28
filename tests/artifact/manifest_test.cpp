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
    ASSERT_EQ(document["format_version"], "2.0");
    document["extra"] = nullptr;
    EXPECT_EQ(artifact::decode_manifest(document.dump(), {}).status().code(), StatusCode::DataLoss);
    document = persistence::parse_json(encoded.value()).value();
    // Legacy pre-reset 1.0 manifest with old descriptor shape must fail closed with Unavailable
    // (checked BEFORE schema validation), never misclassified as DataLoss / corrupt.
    document["format_version"] = "1.0";
    document["descriptors"][0]["data_profile"] = 2;
    EXPECT_EQ(
        artifact::decode_manifest(document.dump(), {}).status().code(), StatusCode::Unavailable);
    document = persistence::parse_json(encoded.value()).value();
    document["format_version"] = "3.0";
    EXPECT_EQ(
        artifact::decode_manifest(document.dump(), {}).status().code(), StatusCode::Unavailable);
    // Duplicate JSON object key must be rejected fail-closed.
    const std::string dup_json = R"({"format_version":"2.0","format_version":"2.0"})";
    EXPECT_EQ(artifact::decode_manifest(dup_json, {}).status().code(), StatusCode::DataLoss);
}

TEST(SegmentManifest, CaptureOwnsBorrowedTextAndComputesSchemaDigestBeforeInputBackingChanges) {
    std::array<char, 5> channel{'/', 'd', 'a', 't', 'a'};
    std::array<std::uint8_t, 3> schema{0, 1, 255};
    rodaq::storage::StreamDescriptor descriptor{};
    descriptor.header.struct_size = sizeof(descriptor);
    descriptor.stream_id = 1;
    descriptor.descriptor_id = 1;
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

TEST(SegmentManifest, DescriptorMetadataNormalizesCompactsAndRoundTripsInManifestV2) {
    const std::string schema = "uint32 value\n";
    const std::string raw_meta = R"( {
        "subscription": { "ros_distribution": "humble", "executor_threads": 2 },
        "offered_qos_profiles": [ { "reliability": 1, "depth": 10 } ]
    } )";
    rodaq::storage::StreamDescriptor descriptor{};
    descriptor.header.struct_size = sizeof(descriptor);
    descriptor.stream_id = 1;
    descriptor.descriptor_id = 1;
    descriptor.channel_name = {"/data", 5};
    descriptor.message_encoding = {"cdr", 3};
    descriptor.schema_name = {"demo/msg/Value", 14};
    descriptor.schema_encoding = {"ros2msg", 7};
    descriptor.schema = {
        reinterpret_cast<const std::uint8_t*>(schema.data()),
        static_cast<std::uint32_t>(schema.size())};
    descriptor.metadata = {
        {"ros2/1.0", 8}, {raw_meta.data(), static_cast<std::uint32_t>(raw_meta.size())}};
    auto captured = artifact::capture_manifest_descriptor(descriptor, 2);
    ASSERT_TRUE(captured.ok()) << captured.status().message();
    EXPECT_EQ(captured.value().metadata_format, "ros2/1.0");
    EXPECT_EQ(
        captured.value().metadata_json,
        R"({"offered_qos_profiles":[{"depth":10,"reliability":1}],"subscription":{"executor_threads":2,"ros_distribution":"humble"}})");
    auto value = make_manifest();
    value.descriptors[0] = std::move(captured.value());
    auto encoded = artifact::encode_manifest(value, {});
    ASSERT_TRUE(encoded.ok()) << encoded.status().message();
    auto decoded = artifact::decode_manifest(encoded.value(), {});
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().descriptors[0].metadata_format, "ros2/1.0");
    EXPECT_EQ(decoded.value().descriptors[0].metadata_json, value.descriptors[0].metadata_json);

    // Duplicate key in descriptor metadata JSON must be rejected.
    const std::string dup_meta = R"({"a":1,"a":2})";
    descriptor.metadata = {
        {"custom/1.0", 10}, {dup_meta.data(), static_cast<std::uint32_t>(dup_meta.size())}};
    EXPECT_EQ(
        artifact::capture_manifest_descriptor(descriptor, 2).status().code(),
        StatusCode::InvalidArgument);
    // Non-object metadata JSON must be rejected.
    const std::string array_meta = R"([1,2,3])";
    descriptor.metadata = {
        {"custom/1.0", 10}, {array_meta.data(), static_cast<std::uint32_t>(array_meta.size())}};
    EXPECT_EQ(
        artifact::capture_manifest_descriptor(descriptor, 2).status().code(),
        StatusCode::InvalidArgument);
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

TEST(SegmentManifest, MultiDescriptorManifestExceeding65536NodesRoundTripsCleanly) {
    // 5 descriptors * 15,000 array elements = >75,000 JSON nodes while total encoded size
    // stays well below 1 MiB. Both encode_manifest and decode_manifest must agree.
    std::string array_json = R"({"a":[)";
    for (int i = 0; i < 15000; ++i) {
        if (i > 0) {
            array_json.push_back(',');
        }
        array_json.push_back('0');
    }
    array_json += "]}";

    auto value = make_manifest();
    const auto base_desc = value.descriptors.front();
    value.descriptors.clear();
    value.coverage.clear();
    for (std::uint64_t i = 1; i <= 5; ++i) {
        auto desc = base_desc;
        desc.stream_id = i;
        desc.descriptor_id = i;
        desc.accepted_records = 1;
        desc.metadata_format = "custom/1.0";
        desc.metadata_json = array_json;
        value.descriptors.push_back(std::move(desc));
        value.coverage.push_back({i, 1, 1, 0, 0, 1, 1});
    }

    artifact::ManifestLimits limits;
    limits.max_bytes = 1024 * 1024;
    auto encoded = artifact::encode_manifest(value, limits);
    ASSERT_TRUE(encoded.ok()) << encoded.status().message();
    EXPECT_LT(encoded.value().size(), limits.max_bytes);
    auto decoded = artifact::decode_manifest(encoded.value(), limits);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().descriptors.size(), 5U);
}

}  // namespace
