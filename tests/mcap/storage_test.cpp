#include "config.hpp"
#include "qos.hpp"
#include "storage.hpp"

#include <gtest/gtest.h>
#include <mcap/reader.hpp>
#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/storage/interface.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace format = rodaq::mcap_storage;
namespace storage = rodaq::storage;
namespace plugin = rodaq::plugin;
namespace fs = std::filesystem;
using rodaq::StatusCode;
using namespace std::chrono_literals;

plugin::Deadline soon() { return plugin::Deadline::after(2s).value(); }

storage::PrepareConfig envelope() {
    storage::PrepareConfig config{};
    config.header.struct_size = sizeof(config);
    config.max_record_bytes = 64;
    config.max_batch_bytes = 256;
    config.max_batch_records = 16;
    config.max_descriptors_per_segment = 8;
    config.max_segment_records = 100;
    config.max_segment_input_bytes = 4096;
    config.max_segment_physical_bytes = 100000;
    config.max_segment_file_count = 1;
    config.max_relative_path_bytes = 64;
    return config;
}

storage::StreamDescriptor make_descriptor(std::uint64_t stream = 1, std::uint64_t id = 1) {
    storage::StreamDescriptor result{};
    result.header.struct_size = sizeof(result);
    result.stream_id = stream;
    result.descriptor_id = id;
    result.channel_name = {"/same", 5};
    result.message_encoding = {"cdr", 3};
    result.schema_name = {"std_msgs/msg/Int32", 18};
    result.schema_encoding = {"ros2msg", 7};
    result.schema = {reinterpret_cast<const std::uint8_t*>("int32 data\n"), 11};
    return result;
}

storage::Record make_record(
    std::uint64_t stream = 1, std::uint64_t id = 1, std::uint64_t recording_time = 1001,
    bool has_publish = false, std::uint64_t publish_time = 0) {
    static const std::array<std::uint8_t, 8> kPayload{0, 1, 0, 0, 5, 0, 0, 0};
    storage::Record result{};
    result.header.struct_size = sizeof(result);
    result.stream_id = stream;
    result.descriptor_id = id;
    result.recording_time = recording_time;
    result.has_publish_time = has_publish ? 1 : 0;
    result.publish_time = has_publish ? publish_time : 0;
    result.payload = {kPayload.data(), kPayload.size()};
    return result;
}

class McapStorageTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> name{};
        const std::string prefix = "/tmp/rodaq-mcap-store-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), name.begin());
        const auto* created = mkdtemp(name.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        fs::create_directory(root_ / "staging");
    }

    void TearDown() override {
        if (manager_) {
            EXPECT_TRUE(manager_->shutdown(soon()).ok());
            manager_.reset();
        }
        std::error_code error;
        fs::remove_all(root_, error);
    }

    void prepare(format::StorageBackend& backend) {
        ASSERT_TRUE(backend.configure(R"({"format_version":"1.0"})").ok());
        ASSERT_TRUE(backend.prepare(envelope(), soon()).ok());
        ASSERT_TRUE(backend.prepare_descriptor(make_descriptor(), soon()).ok());
    }

    void open_segment(format::StorageBackend& backend, std::uint64_t id = 1) {
        const auto path = (root_ / "staging").string();
        storage::SegmentSpec spec{
            {sizeof(storage::SegmentSpec)},
            id,
            {path.data(), static_cast<std::uint32_t>(path.size())}};
        ASSERT_TRUE(backend.open_segment(spec, soon()).ok());
    }

    fs::path root_;
    std::optional<plugin::PluginManager> manager_;
};

TEST(McapConfig, StrictFixedProfileAcceptsDefaultsAndRejectsUnknownsAndUnsupportedOptions) {
    EXPECT_TRUE(format::validate_config(R"({"format_version":"1.0"})").ok());
    EXPECT_TRUE(format::validate_config(R"({"format_version":"1.0","profile":"ros2",
        "ros_distro":"humble","compression":"none","chunk_mode":"none",
        "chunk_crc":false,"message_index":false,"summary":false})")
                    .ok());
    const auto generic_cfg =
        format::parse_config(R"({"format_version":"1.0","profile":"generic"})");
    ASSERT_TRUE(generic_cfg.ok());
    EXPECT_EQ(generic_cfg.value().profile, format::McapProfile::Generic);
    EXPECT_FALSE(generic_cfg.value().ros_distro.has_value());

    for (const auto* input :
         {R"({})", R"({"format_version":"2.0"})", R"({"format_version":"1.0","profile":"unknown"})",
          R"({"format_version":"1.0","profile":"generic","ros_distro":"humble"})",
          R"({"format_version":"1.0","profile":"ros2","ros_distro":"INVALID"})",
          R"({"format_version":"1.0","rosbag2_metadata":"legacy_ros_distro"})",
          R"({"format_version":"1.0","compression":"zstd"})",
          R"({"format_version":"1.0","summary":true})",
          R"({"format_version":"1.0","message_index":0})",
          R"({"format_version":"1.0","chunk_crc":null})",
          R"({"format_version":"1.0","unknown":false})"}) {
        EXPECT_FALSE(format::validate_config(input).ok()) << input;
    }
}

TEST(McapQos, DeterministicVersion8TextFromRos2MetadataPreservesInputOrderAndRejectsInvalid) {
    const rodaq_descriptor_metadata_t empty{};
    EXPECT_EQ(format::parse_ros2_metadata(empty).value().offered_qos_yaml, "[]\n");
    const std::string json =
        R"({"offered_qos_profiles":[{"history":1,"depth":10,"reliability":1,"durability":2,)"
        R"("deadline_sec":0,"deadline_nsec":0,"lifespan_sec":0,"lifespan_nsec":0,)"
        R"("liveliness":1,"lease_sec":0,"lease_nsec":0,"avoid_ros_namespace_conventions":false}],)"
        R"("subscription":{"ros_distribution":"humble"}})";
    const rodaq_descriptor_metadata_t meta{
        {"ros2/1.0", 8}, {json.data(), static_cast<std::uint32_t>(json.size())}};
    const std::string expected =
        "- history: 1\n  depth: 10\n  reliability: 1\n  durability: 2\n"
        "  deadline:\n    sec: 0\n    nsec: 0\n  lifespan:\n    sec: 0\n    nsec: 0\n"
        "  liveliness: 1\n  liveliness_lease_duration:\n    sec: 0\n    nsec: 0\n"
        "  avoid_ros_namespace_conventions: false\n";
    auto parsed = format::parse_ros2_metadata(meta);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    EXPECT_EQ(parsed.value().offered_qos_yaml, expected);
    // Duplicate key in MCAP config and ros2/1.0 metadata must fail closed.
    EXPECT_FALSE(
        format::validate_config(R"({"format_version":"1.0","format_version":"1.0"})").ok());
    const std::string dup = R"({"offered_qos_profiles":[],"offered_qos_profiles":[]})";
    const rodaq_descriptor_metadata_t dup_meta{
        {"ros2/1.0", 8}, {dup.data(), static_cast<std::uint32_t>(dup.size())}};
    EXPECT_FALSE(format::parse_ros2_metadata(dup_meta).ok());
}

TEST_F(McapStorageTest, WritesSingleFileMcapWithPerChannelSequenceAndIndependentReaderDecode) {
    format::StorageBackend backend;
    prepare(backend);
    open_segment(backend, 7);

    std::array<storage::Record, 2> batch{
        make_record(1, 1, 1001, false, 0), make_record(1, 1, 2002, true, 1500)};
    auto outcome = backend.write_batch({batch.data(), batch.size()}, soon());
    ASSERT_TRUE(outcome.status.ok()) << outcome.status.message();
    EXPECT_EQ(outcome.batch_result.confirmed_prefix, 2U);
    EXPECT_EQ(outcome.batch_result.tail_state, RODAQ_STORAGE_TAIL_NONE);

    auto usage = backend.segment_usage();
    ASSERT_TRUE(usage.ok());
    EXPECT_EQ(usage.value().file_count, 1U);
    EXPECT_GT(usage.value().physical_bytes, 0ULL);

    auto sealed = backend.seal_segment(soon());
    ASSERT_TRUE(sealed.ok()) << sealed.status().message();
    ASSERT_EQ(sealed.value().files.size(), 1U);
    EXPECT_EQ(sealed.value().files[0].relative_path, "7.mcap");
    EXPECT_EQ(sealed.value().total_physical_bytes, sealed.value().files[0].physical_size);

    // Independent libmcap reader verification.
    mcap::McapReader reader;
    ASSERT_TRUE(reader.open((root_ / "staging" / "7.mcap").string()).ok());
    std::vector<mcap::Message> messages;
    for (const auto& view : reader.readMessages()) {
        EXPECT_EQ(view.channel->topic, "/same");
        EXPECT_EQ(view.channel->messageEncoding, "cdr");
        EXPECT_EQ(view.schema->name, "std_msgs/msg/Int32");
        EXPECT_EQ(view.schema->encoding, "ros2msg");
        messages.push_back(view.message);
    }
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[0].logTime, 1001ULL);
    EXPECT_EQ(messages[0].publishTime, 1001ULL);
    EXPECT_EQ(messages[0].sequence, 1U);
    EXPECT_EQ(messages[1].logTime, 2002ULL);
    EXPECT_EQ(messages[1].publishTime, 1500ULL);
    EXPECT_EQ(messages[1].sequence, 2U);
}

TEST_F(McapStorageTest, ExpiredDeadlineFailsClosedAcrossPrepareOpenWriteAndSeal) {
    const auto expired = plugin::Deadline::from_native({1}).value();
    format::StorageBackend backend;
    ASSERT_TRUE(backend.configure(R"({"format_version":"1.0"})").ok());
    EXPECT_EQ(backend.prepare(envelope(), expired).status().code(), StatusCode::DeadlineExceeded);
    ASSERT_TRUE(backend.prepare(envelope(), soon()).ok());
    EXPECT_EQ(
        backend.prepare_descriptor(make_descriptor(), expired).status().code(),
        StatusCode::DeadlineExceeded);
    ASSERT_TRUE(backend.prepare_descriptor(make_descriptor(), soon()).ok());

    const auto path = (root_ / "staging").string();
    storage::SegmentSpec spec{
        {sizeof(storage::SegmentSpec)}, 3, {path.data(), static_cast<std::uint32_t>(path.size())}};
    EXPECT_EQ(backend.open_segment(spec, expired).status().code(), StatusCode::DeadlineExceeded);
    ASSERT_TRUE(backend.open_segment(spec, soon()).ok());

    auto rec = make_record();
    auto write_expired = backend.write_batch({&rec, 1}, expired);
    EXPECT_EQ(write_expired.status.code(), StatusCode::DeadlineExceeded);
    EXPECT_EQ(write_expired.batch_result.confirmed_prefix, 0U);
    EXPECT_EQ(write_expired.batch_result.tail_state, RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN);

    ASSERT_TRUE(backend.write_batch({&rec, 1}, soon()).status.ok());
    EXPECT_EQ(backend.seal_segment(expired).status().code(), StatusCode::DeadlineExceeded);
    EXPECT_TRUE(backend.quiesce(soon()).ok());
}

TEST_F(McapStorageTest, SinkWriteFailureReportsUnknownTailAndPluginPackageLoadsViaManager) {
    bool fail_write = false;
    format::StorageBackend backend([&](int fd, const void* data, std::size_t size) -> ssize_t {
        if (fail_write) {
            errno = EIO;
            return -1;
        }
        return ::write(fd, data, size);
    });
    prepare(backend);
    open_segment(backend, 9);
    auto rec = make_record();
    fail_write = true;
    auto failed = backend.write_batch({&rec, 1}, soon());
    EXPECT_FALSE(failed.status.ok());
    EXPECT_EQ(failed.batch_result.tail_state, RODAQ_STORAGE_TAIL_UNKNOWN);

    // Load actual installed MCAP DSO through PluginManager.
    plugin::PluginManagerOptions opts;
    opts.search_paths = {RODAQ_MCAP_PACKAGE};
    opts.max_loaded_plugins = 1;
    opts.max_instances = 1;
    opts.max_config_size = 4096;
    opts.shutdown_timeout = 2s;
    auto mgr = plugin::PluginManager::create(std::move(opts));
    ASSERT_TRUE(mgr.ok()) << mgr.status().message();
    manager_.emplace(std::move(mgr.value()));
    auto lib = manager_->load("com.rodaq.storage.mcap", "1.0.0");
    ASSERT_TRUE(lib.ok()) << lib.status().message();
    auto inst = lib.value().create_instance(R"({"format_version":"1.0"})");
    ASSERT_TRUE(inst.ok()) << inst.status().message();
    auto handle = inst.value().query<storage::Interface>();
    ASSERT_TRUE(handle.ok());
    ASSERT_TRUE(handle.value().prepare(envelope(), soon()).ok());
    ASSERT_TRUE(handle.value().quiesce(soon()).ok());
    ASSERT_TRUE(inst.value().close(soon()).ok());
}

TEST_F(McapStorageTest, GenericProfileWritesArbitraryEncodingOptionalSchemaAndNoRosbag2Metadata) {
    format::StorageBackend backend;
    ASSERT_TRUE(backend.configure(R"({"format_version":"1.0","profile":"generic"})").ok());
    ASSERT_TRUE(backend.prepare(envelope(), soon()).ok());

    // Stream 1: schemaless JSON payload (allowed in generic profile).
    storage::StreamDescriptor schemaless{};
    schemaless.header.struct_size = sizeof(schemaless);
    schemaless.stream_id = 1;
    schemaless.descriptor_id = 1;
    schemaless.channel_name = {"/telemetry", 10};
    schemaless.message_encoding = {"json", 4};
    ASSERT_TRUE(backend.prepare_descriptor(schemaless, soon()).ok());

    // Stream 2: custom jsonschema payload with custom metadata (ignored by generic MCAP).
    const std::string schema_text = R"({"type":"object"})";
    const std::string custom_meta = R"({"sensor":"imu"})";
    storage::StreamDescriptor with_schema{};
    with_schema.header.struct_size = sizeof(with_schema);
    with_schema.stream_id = 2;
    with_schema.descriptor_id = 2;
    with_schema.channel_name = {"/imu", 4};
    with_schema.message_encoding = {"json", 4};
    with_schema.schema_name = {"ImuSample", 9};
    with_schema.schema_encoding = {"jsonschema", 10};
    with_schema.schema = {
        reinterpret_cast<const std::uint8_t*>(schema_text.data()),
        static_cast<std::uint32_t>(schema_text.size())};
    with_schema.metadata = {
        {"custom/1.0", 10}, {custom_meta.data(), static_cast<std::uint32_t>(custom_meta.size())}};
    ASSERT_TRUE(backend.prepare_descriptor(with_schema, soon()).ok());

    open_segment(backend, 41);
    std::array<storage::Record, 2> batch{
        make_record(1, 1, 1000, false, 0), make_record(2, 2, 2000, true, 1900)};
    ASSERT_TRUE(backend.write_batch({batch.data(), batch.size()}, soon()).status.ok());
    ASSERT_TRUE(backend.seal_segment(soon()).ok());

    mcap::McapReader reader;
    ASSERT_TRUE(reader.open((root_ / "staging" / "41.mcap").string()).ok());
    std::vector<std::string> topics;
    for (const auto& view : reader.readMessages()) {
        topics.push_back(view.channel->topic);
        if (view.channel->topic == "/telemetry") {
            EXPECT_EQ(view.channel->schemaId, 0U);
            EXPECT_EQ(view.schema, nullptr);
        } else {
            ASSERT_NE(view.schema, nullptr);
            EXPECT_EQ(view.schema->name, "ImuSample");
            EXPECT_EQ(view.schema->encoding, "jsonschema");
        }
    }
    ASSERT_EQ(topics.size(), 2U);
    ASSERT_TRUE(reader.header().has_value());
    EXPECT_EQ(reader.header()->profile, "");
    bool saw_rosbag2 = false;
    mcap::TypedRecordReader records(*reader.dataSource(), 8);
    records.onMetadata = [&](const mcap::Metadata& meta, mcap::ByteOffset) {
        if (meta.name == "rosbag2") {
            saw_rosbag2 = true;
        }
    };
    while (records.next()) {
    }
    EXPECT_TRUE(records.status().ok());
    EXPECT_FALSE(saw_rosbag2);
}

TEST_F(McapStorageTest, Ros2DistroDecisionTableCoversExplicitAgreedConflictingAndMissingCases) {
    auto read_ros_distro = [&](const fs::path& file) -> std::optional<std::string> {
        mcap::McapReader reader;
        EXPECT_TRUE(reader.open(file.string()).ok());
        for (const auto& view : reader.readMessages()) {
            (void)view;
        }
        EXPECT_TRUE(reader.header().has_value());
        EXPECT_EQ(reader.header()->profile, "ros2");
        std::optional<std::string> found;
        mcap::TypedRecordReader records(*reader.dataSource(), 8);
        records.onMetadata = [&](const mcap::Metadata& meta, mcap::ByteOffset) {
            if (meta.name == "rosbag2") {
                auto it = meta.metadata.find("ROS_DISTRO");
                if (it != meta.metadata.end()) {
                    found = it->second;
                }
            }
        };
        while (records.next()) {
        }
        EXPECT_TRUE(records.status().ok());
        return found;
    };

    const std::string meta_humble = R"({"subscription":{"ros_distribution":"humble"}})";
    const std::string meta_jazzy = R"({"subscription":{"ros_distribution":"jazzy"}})";

    // Cell 3: ros2 + explicit ros_distro + descriptor without ros_distribution -> writes explicit.
    {
        format::StorageBackend b;
        ASSERT_TRUE(
            b.configure(R"({"format_version":"1.0","profile":"ros2","ros_distro":"humble"})").ok());
        ASSERT_TRUE(b.prepare(envelope(), soon()).ok());
        ASSERT_TRUE(b.prepare_descriptor(make_descriptor(1, 1), soon()).ok());
        open_segment(b, 51);
        auto r = make_record(1, 1, 100);
        ASSERT_TRUE(b.write_batch({&r, 1}, soon()).status.ok());
        ASSERT_TRUE(b.seal_segment(soon()).ok());
        EXPECT_EQ(
            read_ros_distro(root_ / "staging" / "51.mcap"), std::optional<std::string>{"humble"});
    }

    // Cell 4 & Cell 5: ros2 + explicit ros_distro -> matching succeeds, conflicting returns
    // Conflict.
    {
        format::StorageBackend b;
        ASSERT_TRUE(
            b.configure(R"({"format_version":"1.0","profile":"ros2","ros_distro":"humble"})").ok());
        ASSERT_TRUE(b.prepare(envelope(), soon()).ok());
        auto d_match = make_descriptor(1, 1);
        d_match.metadata = {
            {"ros2/1.0", 8}, {meta_humble.data(), static_cast<std::uint32_t>(meta_humble.size())}};
        ASSERT_TRUE(b.prepare_descriptor(d_match, soon()).ok());

        auto d_conflict = make_descriptor(2, 2);
        d_conflict.metadata = {
            {"ros2/1.0", 8}, {meta_jazzy.data(), static_cast<std::uint32_t>(meta_jazzy.size())}};
        EXPECT_EQ(b.prepare_descriptor(d_conflict, soon()).status().code(), StatusCode::Conflict);

        open_segment(b, 52);
        auto r = make_record(1, 1, 100);
        ASSERT_TRUE(b.write_batch({&r, 1}, soon()).status.ok());
        ASSERT_TRUE(b.seal_segment(soon()).ok());
        EXPECT_EQ(
            read_ros_distro(root_ / "staging" / "52.mcap"), std::optional<std::string>{"humble"});
    }

    // Cell 6: ros2 + no explicit ros_distro + all descriptors agree -> auto-emits agreed distro.
    {
        format::StorageBackend b;
        ASSERT_TRUE(b.configure(R"({"format_version":"1.0","profile":"ros2"})").ok());
        ASSERT_TRUE(b.prepare(envelope(), soon()).ok());
        auto d1 = make_descriptor(1, 1);
        d1.metadata = {
            {"ros2/1.0", 8}, {meta_humble.data(), static_cast<std::uint32_t>(meta_humble.size())}};
        auto d2 = make_descriptor(2, 2);
        d2.metadata = {
            {"ros2/1.0", 8}, {meta_humble.data(), static_cast<std::uint32_t>(meta_humble.size())}};
        ASSERT_TRUE(b.prepare_descriptor(d1, soon()).ok());
        ASSERT_TRUE(b.prepare_descriptor(d2, soon()).ok());
        open_segment(b, 53);
        auto r = make_record(1, 1, 100);
        ASSERT_TRUE(b.write_batch({&r, 1}, soon()).status.ok());
        ASSERT_TRUE(b.seal_segment(soon()).ok());
        EXPECT_EQ(
            read_ros_distro(root_ / "staging" / "53.mcap"), std::optional<std::string>{"humble"});
    }

    // Cell 7a: ros2 + no explicit ros_distro + missing descriptor distro -> omits rosbag2 metadata.
    {
        format::StorageBackend b;
        ASSERT_TRUE(b.configure(R"({"format_version":"1.0","profile":"ros2"})").ok());
        ASSERT_TRUE(b.prepare(envelope(), soon()).ok());
        ASSERT_TRUE(b.prepare_descriptor(make_descriptor(1, 1), soon()).ok());
        open_segment(b, 54);
        auto r = make_record(1, 1, 100);
        ASSERT_TRUE(b.write_batch({&r, 1}, soon()).status.ok());
        ASSERT_TRUE(b.seal_segment(soon()).ok());
        EXPECT_FALSE(read_ros_distro(root_ / "staging" / "54.mcap").has_value());
    }

    // Cell 7b: ros2 + no explicit ros_distro + disagreeing descriptor distros -> omits rosbag2
    // metadata.
    {
        format::StorageBackend b;
        ASSERT_TRUE(b.configure(R"({"format_version":"1.0","profile":"ros2"})").ok());
        ASSERT_TRUE(b.prepare(envelope(), soon()).ok());
        auto d1 = make_descriptor(1, 1);
        d1.metadata = {
            {"ros2/1.0", 8}, {meta_humble.data(), static_cast<std::uint32_t>(meta_humble.size())}};
        auto d2 = make_descriptor(2, 2);
        d2.metadata = {
            {"ros2/1.0", 8}, {meta_jazzy.data(), static_cast<std::uint32_t>(meta_jazzy.size())}};
        ASSERT_TRUE(b.prepare_descriptor(d1, soon()).ok());
        ASSERT_TRUE(b.prepare_descriptor(d2, soon()).ok());
        open_segment(b, 55);
        auto r = make_record(1, 1, 100);
        ASSERT_TRUE(b.write_batch({&r, 1}, soon()).status.ok());
        ASSERT_TRUE(b.seal_segment(soon()).ok());
        EXPECT_FALSE(read_ros_distro(root_ / "staging" / "55.mcap").has_value());
    }
}

}  // namespace
