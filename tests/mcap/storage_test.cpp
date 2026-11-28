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
    result.data_profile = RODAQ_DATA_PROFILE_ROS2;
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
    EXPECT_TRUE(format::validate_config(R"({"format_version":"1.0","compression":"none",
        "chunk_mode":"none","chunk_crc":false,"message_index":false,"summary":false,
        "rosbag2_metadata":"legacy_ros_distro"})")
                    .ok());
    for (const auto* input :
         {R"({})", R"({"format_version":"2.0"})",
          R"({"format_version":"1.0","compression":"zstd"})",
          R"({"format_version":"1.0","summary":true})",
          R"({"format_version":"1.0","message_index":0})",
          R"({"format_version":"1.0","chunk_crc":null})",
          R"({"format_version":"1.0","unknown":false})"}) {
        EXPECT_FALSE(format::validate_config(input).ok()) << input;
    }
}

TEST(McapQos, DeterministicVersion8TextPreservesInputOrder) {
    EXPECT_EQ(format::encode_qos(nullptr, 0).value(), "[]\n");
    rodaq_ros2_qos_profile_t qos{};
    qos.history = 1;
    qos.depth = 10;
    qos.reliability = 1;
    qos.durability = 2;
    qos.liveliness = 1;
    const std::string expected =
        "- history: 1\n  depth: 10\n  reliability: 1\n  durability: 2\n"
        "  deadline:\n    sec: 0\n    nsec: 0\n  lifespan:\n    sec: 0\n    nsec: 0\n"
        "  liveliness: 1\n  liveliness_lease_duration:\n    sec: 0\n    nsec: 0\n"
        "  avoid_ros_namespace_conventions: false\n";
    EXPECT_EQ(format::encode_qos(&qos, 1).value(), expected);
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

}  // namespace
