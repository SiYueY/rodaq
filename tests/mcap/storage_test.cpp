#include "storage.hpp"
#include "config.hpp"
#include "qos.hpp"
#include "rodaqmeta.hpp"

#include <gtest/gtest.h>
#include <mcap/reader.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/registry.hpp>
#include <rodaq/storage/interface.hpp>
#include <rodaq/storage/capacity.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <tuple>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

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
    config.max_record_bytes = 8;
    config.max_batch_bytes = 128;
    config.max_batch_records = 16;
    config.max_descriptors_per_segment = 8;
    config.max_segment_records = 100;
    config.max_segment_input_bytes = 800;
    config.max_segment_physical_bytes = 100000;
    config.max_segment_file_count = 2;
    config.max_relative_path_bytes = 64;
    config.system_boot_id = 4;
    config.playback_anchor_monotonic = 100;
    config.playback_anchor_realtime = 1000;
    return config;
}

storage::StreamDescriptor descriptor(std::uint64_t stream = 1, std::uint64_t id = 1) {
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

storage::Record record(std::uint64_t stream = 1, std::uint64_t id = 1) {
    static const std::array<std::uint8_t, 8> kPayload{0, 1, 0, 0, 5, 0, 0, 0};
    storage::Record result{};
    result.header.struct_size = sizeof(result);
    result.stream_id = stream;
    result.descriptor_id = id;
    result.acceptance_sequence = 0x123456789abcdef0;
    result.system_boot_id = 4;
    result.clock_epoch_id = 1;
    result.receive_monotonic = 101;
    result.receive_realtime = 1001;
    result.payload = {kPayload.data(), kPayload.size()};
    return result;
}

std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
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
            auto stopped = manager_->shutdown(soon());
            EXPECT_TRUE(stopped.ok());
            if (!stopped.ok()) {
                return;
            }
            manager_.reset();
        }
        std::error_code error;
        for (const auto& entry : fs::recursive_directory_iterator(root_, error)) {
            fs::permissions(entry.path(), fs::perms::owner_all, error);
        }
        fs::remove_all(root_, error);
        EXPECT_FALSE(error) << error.message();
    }

    void prepare(format::StorageBackend& backend) {
        ASSERT_TRUE(backend.configure(R"({"format_version":"1.0"})").ok());
        auto prepared = backend.prepare(envelope(), soon());
        ASSERT_TRUE(prepared.ok());
        model_ = prepared.value();
        ASSERT_TRUE(backend.prepare_descriptor(descriptor(), soon()).ok());
    }

    void open(format::StorageBackend& backend, std::uint64_t id = 1) {
        const auto path = (root_ / "staging").string();
        storage::SegmentSpec spec{
            {sizeof(storage::SegmentSpec)},
            id,
            {path.data(), static_cast<std::uint32_t>(path.size())}};
        ASSERT_TRUE(backend.open_segment(spec, soon()).ok());
    }

    fs::path root_;
    std::optional<plugin::PluginManager> manager_;
    storage::CapacityModel model_{};
};

TEST(
    McapConfig,
    StrictFixedProfileAcceptsDefaultsAndRejectsDuplicatesUnknownsAndUnsupportedOptions) {
    EXPECT_TRUE(format::validate_config(R"({"format_version":"1.0"})").ok());
    EXPECT_TRUE(format::validate_config(R"({"format_version":"1.0","compression":"none",
        "chunk_mode":"none","chunk_crc":false,"message_index":false,"summary":false,
        "rosbag2_metadata":"legacy_ros_distro"})")
                    .ok());
    for (const auto input :
         {R"({})", R"({"format_version":"2.0"})",
          R"({"format_version":"1.0","format_version":"1.0"})",
          R"({"format_version":"1.0","compression":"zstd"})",
          R"({"format_version":"1.0","summary":true})",
          R"({"format_version":"1.0","message_index":0})",
          R"({"format_version":"1.0","chunk_crc":null})",
          R"({"format_version":"1.0","unknown":false})"}) {
        EXPECT_FALSE(format::validate_config(input).ok()) << input;
    }
}

TEST(McapQos, DeterministicVersion8TextAndExactWorstCaseBoundPreserveInputOrder) {
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
    auto maximum = qos;
    maximum.depth = std::numeric_limits<std::uint64_t>::max();
    maximum.deadline = {maximum.depth, 999999999};
    maximum.lifespan = maximum.deadline;
    maximum.liveliness_lease_duration = maximum.deadline;
    const auto largest = format::encode_qos(&maximum, 1).value();
    EXPECT_EQ(largest.size(), format::kMaxEncodedQosBytes);
    const std::array<rodaq_ros2_qos_profile_t, 2> ordered{maximum, qos};
    EXPECT_EQ(format::encode_qos(ordered.data(), ordered.size()).value(), largest + expected);
    maximum.deadline.nsec = 1000000000;
    EXPECT_FALSE(format::encode_qos(&maximum, 1).ok());
}

TEST_F(
    McapStorageTest, CompositeFilesPreserveCanonicalSequenceAndStablePlaybackAcrossWallClockSteps) {
    format::StorageBackend backend;
    prepare(backend);
    open(backend);
    const auto initial = backend.segment_usage().value();
    const auto model = model_;
    EXPECT_EQ(initial.physical_bytes, model.fixed_segment_overhead_bytes);
    std::array<storage::Record, 3> records{record(), record(), record()};
    records[1].acceptance_sequence++;
    records[1].receive_monotonic = 102;
    records[1].receive_realtime = -1000;
    records[1].clock_epoch_id = 2;
    records[2].acceptance_sequence += 2;
    records[2].receive_monotonic = 103;
    records[2].receive_realtime = 50000;
    records[2].timestamp_count = 1;
    records[2].timestamps[0] = {
        RODAQ_TIMESTAMP_ROLE_SOURCE_PUBLICATION, RODAQ_CLOCK_DOMAIN_UNIX_EPOCH, 0, 900, 0, 0};
    const auto outcome = backend.write_batch({records.data(), records.size()}, soon());
    ASSERT_TRUE(outcome.status.ok());
    EXPECT_EQ(outcome.batch_result.confirmed_prefix, 3);
    EXPECT_EQ(outcome.batch_result.tail_state, RODAQ_STORAGE_TAIL_NONE);
    const auto before_seal = backend.segment_usage().value();
    EXPECT_EQ(before_seal.pending_physical_upper_bound, 0);
    const auto sealed = backend.seal_segment(soon());
    ASSERT_TRUE(sealed.ok());
    EXPECT_EQ(sealed.value().files.size(), 2);
    EXPECT_EQ(
        sealed.value().total_physical_bytes - before_seal.physical_bytes, model.seal_reserve_bytes);
    mcap::McapReader reader;
    ASSERT_TRUE(reader.open((root_ / "staging/1.mcap").string()).ok());
    std::size_t count = 0;
    for (const auto& view : reader.readMessages()) {
        ASSERT_LT(count, records.size());
        EXPECT_EQ(view.message.logTime, 1001 + count);
        EXPECT_EQ(view.message.publishTime, count == 2 ? 900 : 1001 + count);
        EXPECT_EQ(view.message.sequence, 0);
        EXPECT_EQ(view.message.dataSize, records[count].payload.size);
        EXPECT_EQ(view.message.data[4], std::byte{5});
        ++count;
    }
    EXPECT_EQ(count, records.size());
    const auto meta = read(root_ / "staging/1.rodaqmeta");
    auto header = format::decode_meta_header(meta).value();
    std::size_t offset = header.size;
    for (std::size_t i = 0; i < records.size(); ++i) {
        auto parsed =
            format::decode_meta_entry(std::string_view(meta).substr(offset), header.minor);
        ASSERT_TRUE(parsed.ok());
        EXPECT_EQ(parsed.value().ordinal, i);
        EXPECT_EQ(parsed.value().record.acceptance_sequence, records[i].acceptance_sequence);
        EXPECT_EQ(parsed.value().record.receive_realtime, records[i].receive_realtime);
        EXPECT_EQ(parsed.value().record.clock_epoch_id, records[i].clock_epoch_id);
        offset += parsed.value().size;
    }
    const auto footer = format::decode_meta_footer(std::string_view(meta).substr(offset)).value();
    EXPECT_EQ(footer.entry_count, count);
    EXPECT_EQ(footer.bytes_before_footer, offset);
    EXPECT_EQ(offset + format::kMetaFooterBytes, meta.size());
    ASSERT_TRUE(backend.quiesce(soon()).ok());
    EXPECT_EQ(read(root_ / "staging/1.rodaqmeta"), meta);
}

TEST_F(McapStorageTest, SameDescriptorIdAcrossStreamsNeverReusesTheWrongSchemaOrChannel) {
    format::StorageBackend backend;
    prepare(backend);
    auto second = descriptor(2);
    second.schema_name = {"std_msgs/msg/UInt32", 19};
    second.schema = {reinterpret_cast<const std::uint8_t*>("uint32 data\n"), 12};
    ASSERT_TRUE(backend.prepare_descriptor(second, soon()).ok());
    ASSERT_TRUE(backend.prepare_descriptor(descriptor(1, 2), soon()).ok());
    open(backend);
    std::array<storage::Record, 4> records{record(), record(2), record(1, 2), record()};
    ASSERT_TRUE(backend.write_batch({records.data(), records.size()}, soon()).status.ok());
    ASSERT_TRUE(backend.seal_segment(soon()).ok());
    mcap::McapReader reader;
    ASSERT_TRUE(reader.open((root_ / "staging/1.mcap").string()).ok());
    std::vector<mcap::SchemaId> schemas;
    std::vector<mcap::ChannelId> channels;
    for (const auto& view : reader.readMessages()) {
        schemas.push_back(view.schema->id);
        channels.push_back(view.channel->id);
    }
    ASSERT_EQ(schemas.size(), 4);
    EXPECT_NE(schemas[0], schemas[1]);
    EXPECT_EQ(schemas[0], schemas[2]);
    EXPECT_EQ(channels[0], channels[3]);
    EXPECT_NE(channels[0], channels[1]);
    EXPECT_NE(channels[0], channels[2]);
}

TEST_F(McapStorageTest, SealedFilesStayImmutableWhileCachedDescriptorsServeANewSegment) {
    format::StorageBackend backend;
    prepare(backend);
    open(backend);
    auto input = record();
    ASSERT_TRUE(backend.write_batch({&input, 1}, soon()).status.ok());
    ASSERT_TRUE(backend.seal_segment(soon()).ok());
    const auto first = read(root_ / "staging/1.mcap");
    const auto meta = read(root_ / "staging/1.rodaqmeta");
    open(backend, 2);
    ASSERT_TRUE(backend.write_batch({&input, 1}, soon()).status.ok());
    ASSERT_TRUE(backend.seal_segment(soon()).ok());
    EXPECT_EQ(read(root_ / "staging/1.mcap"), first);
    EXPECT_EQ(read(root_ / "staging/1.rodaqmeta"), meta);
    mcap::McapReader reader;
    ASSERT_TRUE(reader.open((root_ / "staging/2.mcap").string()).ok());
    unsigned count = 0;
    for (const auto& view : reader.readMessages()) {
        EXPECT_EQ(view.schema->name, "std_msgs/msg/Int32");
        ++count;
    }
    EXPECT_EQ(count, 1);
}

TEST_F(McapStorageTest, SidecarFailureConfirmsOnlyTheAlreadyPairedPrefixAndNeverSealsNormally) {
    unsigned meta_calls = 0;
    format::StorageBackend backend(
        {}, [&](int fd, const void* bytes, std::size_t count) -> ssize_t {
            if (++meta_calls == 3) {  // Header, Entry 0, failing Entry 1.
                errno = ENOSPC;
                return -1;
            }
            return ::write(fd, bytes, count);
        });
    prepare(backend);
    open(backend);
    std::array<storage::Record, 3> records{record(), record(), record()};
    const auto written = backend.write_batch({records.data(), records.size()}, soon());
    EXPECT_EQ(written.status.code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(written.batch_result.confirmed_prefix, 1);
    EXPECT_EQ(written.batch_result.tail_state, RODAQ_STORAGE_TAIL_UNKNOWN);
    EXPECT_FALSE(backend.seal_segment(soon()).ok());
    const auto size = backend.segment_usage().value().physical_bytes;
    EXPECT_FALSE(backend.write_batch({records.data(), 1}, soon()).status.ok());
    EXPECT_EQ(backend.segment_usage().value().physical_bytes, size);
    EXPECT_EQ(meta_calls, 3);
    EXPECT_TRUE(backend.quiesce(soon()).ok());
    EXPECT_EQ(fs::file_size(root_ / "staging/1.rodaqmeta"), 24 + 80);
    EXPECT_TRUE(fs::exists(root_ / "staging/1.mcap"));
}

TEST_F(McapStorageTest, InvalidBootOrUnrepresentableTimeFailsBeforeAnyBatchMutation) {
    format::StorageBackend backend;
    prepare(backend);
    open(backend);
    const auto initial = backend.segment_usage().value().physical_bytes;
    auto input = record();
    for (unsigned mode = 0; mode < 3; ++mode) {
        input = record();
        if (mode == 0) {
            input.system_boot_id = 99;
        }
        if (mode == 1) {
            input.receive_monotonic = 99;
        }
        if (mode == 2) {
            input.receive_monotonic = std::numeric_limits<std::uint64_t>::max();
        }
        const auto outcome = backend.write_batch({&input, 1}, soon());
        EXPECT_FALSE(outcome.status.ok());
        EXPECT_EQ(outcome.batch_result.confirmed_prefix, 0);
        EXPECT_EQ(outcome.batch_result.tail_state, RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN);
        EXPECT_EQ(backend.segment_usage().value().physical_bytes, initial);
    }
}

TEST_F(McapStorageTest, NumericIdAndFileEnvelopeLimitsFailDuringPrepare) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        format::StorageBackend backend;
        ASSERT_TRUE(backend.configure(R"({"format_version":"1.0"})").ok());
        auto config = envelope();
        if (mode == 0) {
            config.max_descriptors_per_segment = 65536;
        }
        if (mode == 1) {
            config.max_segment_file_count = 1;
        }
        if (mode == 2) {
            config.max_relative_path_bytes = 29;
        }
        EXPECT_EQ(backend.prepare(config, soon()).status().code(), StatusCode::ResourceExhausted);
        EXPECT_TRUE(fs::is_empty(root_ / "staging"));
    }
}

TEST_F(McapStorageTest, MaximumSourceAndTimestampFactsExactlyReachTheDeclaredRecordBound) {
    format::StorageBackend backend;
    prepare(backend);
    open(backend);
    auto input = record();
    ASSERT_TRUE(backend.write_batch({&input, 1}, soon()).status.ok());
    const auto initial = backend.segment_usage().value().physical_bytes;
    input.has_source_id = 1;
    input.source_id.kind = RODAQ_SOURCE_ID_ROS2_GID;
    input.source_id.size = RODAQ_SOURCE_ID_DATA_CAPACITY;
    std::fill(std::begin(input.source_id.data), std::end(input.source_id.data), 1);
    input.has_source_session_id = 1;
    input.source_session_id = 8;
    input.has_source_sequence = 1;
    input.source_sequence = 10;
    input.sequence_scope = RODAQ_SEQUENCE_SCOPE_SOURCE_SESSION;
    input.timestamp_count = RODAQ_STORAGE_TIMESTAMP_CAPACITY;
    for (auto& timestamp : input.timestamps) {
        timestamp = {
            RODAQ_TIMESTAMP_ROLE_SOURCE_PUBLICATION, RODAQ_CLOCK_DOMAIN_UNIX_EPOCH, 0, 900, 0, 0};
    }
    ++input.acceptance_sequence;
    ASSERT_TRUE(backend.write_batch({&input, 1}, soon()).status.ok());
    EXPECT_EQ(
        backend.segment_usage().value().physical_bytes - initial,
        storage::record_bound(model_, input.payload.size).value());
    EXPECT_TRUE(backend.seal_segment(soon()).ok());
}

TEST_F(McapStorageTest, MaximumSchemaAndQosMetadataStayInsideTheDescriptorExpansionProof) {
    format::StorageBackend backend;
    ASSERT_TRUE(backend.configure(R"({"format_version":"1.0"})").ok());
    auto limits = envelope();
    limits.max_segment_physical_bytes = 16 * 1024 * 1024;
    const auto prepared = backend.prepare(limits, soon());
    ASSERT_TRUE(prepared.ok());
    auto input = descriptor();
    std::string schema(1024 * 1024, '#');
    schema.replace(0, 11, "int32 data\n");
    schema.back() = '\n';
    input.schema = {
        reinterpret_cast<const std::uint8_t*>(schema.data()),
        static_cast<std::uint32_t>(schema.size())};
    std::vector<rodaq_ros2_qos_profile_t> profiles(format::kMaxQosProfiles);
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        profiles[i].depth = std::numeric_limits<std::uint64_t>::max() - profiles.size() + i;
        profiles[i].deadline = {std::numeric_limits<std::uint64_t>::max(), 999999999};
        profiles[i].lifespan = profiles[i].deadline;
        profiles[i].liveliness_lease_duration = profiles[i].deadline;
    }
    input.ros2_offered_qos_profiles = profiles.data();
    input.ros2_offered_qos_profile_count = static_cast<std::uint32_t>(profiles.size());
    ASSERT_TRUE(backend.prepare_descriptor(input, soon()).ok());
    open(backend);
    const auto initial = backend.segment_usage().value().physical_bytes;
    auto value = record();
    ASSERT_TRUE(backend.write_batch({&value, 1}, soon()).status.ok());
    const auto input_bytes = input.channel_name.size + input.message_encoding.size +
                             input.schema_name.size + input.schema_encoding.size +
                             input.schema.size + profiles.size() * sizeof(rodaq_ros2_qos_profile_t);
    EXPECT_LE(
        backend.segment_usage().value().physical_bytes - initial,
        storage::descriptor_bound(prepared.value(), input_bytes).value() +
            storage::record_bound(prepared.value(), value.payload.size).value());
    EXPECT_EQ(backend.segment_usage().value().pending_physical_upper_bound, 0);
    auto sealed = backend.seal_segment(soon());
    ASSERT_TRUE(sealed.ok());
    EXPECT_EQ(
        sealed.value().total_physical_bytes,
        fs::file_size(root_ / "staging/1.mcap") + fs::file_size(root_ / "staging/1.rodaqmeta"));
}

TEST_F(McapStorageTest, FullNumericSchemaAndChannelNamespaceNeverWrapsAndResetsPerSegment) {
    format::StorageBackend backend;
    ASSERT_TRUE(backend.configure(R"({"format_version":"1.0"})").ok());
    auto config = envelope();
    config.max_descriptors_per_segment = 65535;
    config.max_segment_records = 65535;
    config.max_segment_input_bytes = 65535 * 8;
    config.max_segment_physical_bytes = 256 * 1024 * 1024;
    ASSERT_TRUE(backend.prepare(config, soon()).ok());
    for (std::uint64_t id = 1; id <= 65535; ++id) {
        auto input = descriptor(1, id);
        const auto name = "test_msgs/msg/T" + std::to_string(id);
        input.schema_name = {name.data(), static_cast<std::uint32_t>(name.size())};
        ASSERT_TRUE(backend.prepare_descriptor(input, soon()).ok()) << id;
    }
    EXPECT_EQ(
        backend.prepare_descriptor(descriptor(1, 65536), soon()).status().code(),
        StatusCode::ResourceExhausted);
    open(backend);
    std::array<storage::Record, 16> records;
    for (std::uint64_t first = 1; first <= 65535; first += records.size()) {
        const auto count =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(records.size(), 65536 - first));
        for (std::uint32_t i = 0; i < count; ++i) {
            records[i] = record(1, first + i);
            records[i].acceptance_sequence = first + i;
        }
        ASSERT_TRUE(backend.write_batch({records.data(), count}, soon()).status.ok()) << first;
    }
    ASSERT_TRUE(backend.seal_segment(soon()).ok());
    {
        mcap::McapReader reader;
        ASSERT_TRUE(reader.open((root_ / "staging/1.mcap").string()).ok());
        std::uint64_t count = 0;
        std::vector<bool> channels(65536, false);
        for (const auto& view : reader.readMessages()) {
            ++count;
            EXPECT_FALSE(channels[view.channel->id]);
            channels[view.channel->id] = true;
            EXPECT_EQ(view.schema->id, count);
        }
        EXPECT_EQ(count, 65535);
    }
    open(backend, 2);
    auto last = record(1, 65535);
    ASSERT_TRUE(backend.write_batch({&last, 1}, soon()).status.ok());
    ASSERT_TRUE(backend.seal_segment(soon()).ok());
    mcap::McapReader reader;
    ASSERT_TRUE(reader.open((root_ / "staging/2.mcap").string()).ok());
    unsigned last_count = 0;
    for (const auto& view : reader.readMessages()) {
        ++last_count;
        EXPECT_EQ(view.schema->id, 1);
        EXPECT_EQ(view.schema->name, "test_msgs/msg/T65535");
    }
    EXPECT_EQ(last_count, 1);
}

// Reference offsets come from the standard MCAP record framing, independently
// of the backend's write-call fragmentation and capacity implementation.
std::vector<std::uint64_t> message_offsets(const std::string& file) {
    std::vector<std::uint64_t> offsets;
    std::size_t offset = 8;
    while (offset <= file.size() && file.size() - offset >= 9) {
        const auto opcode = static_cast<unsigned char>(file[offset]);
        std::uint64_t size = 0;
        for (unsigned i = 0; i < 8; ++i) {
            size |= static_cast<std::uint64_t>(static_cast<unsigned char>(file[offset + 1 + i]))
                    << (8 * i);
        }
        if (size > file.size() - offset - 9) {
            break;
        }
        if (opcode == 5) {
            offsets.push_back(offset);
        }
        offset += 9 + static_cast<std::size_t>(size);
    }
    return offsets;
}

class McapCompositeFaultTest
: public McapStorageTest,
  public testing::WithParamInterface<std::tuple<bool, unsigned, unsigned>> {};

TEST_P(
    McapCompositeFaultTest, ExactPairedPrefixSurvivesEitherFileFaultAndUnknownTailIsNeverRetried) {
    const auto mcap_fault = std::get<0>(GetParam());
    const auto mode = std::get<1>(GetParam());
    const auto prefix = std::get<2>(GetParam());
    std::array<storage::Record, 3> records{record(), record(), record()};
    for (unsigned i = 0; i < records.size(); ++i) {
        records[i].acceptance_sequence += i;
    }
    {
        format::StorageBackend reference;
        prepare(reference);
        open(reference);
        ASSERT_TRUE(reference.write_batch({records.data(), records.size()}, soon()).status.ok());
        ASSERT_TRUE(reference.seal_segment(soon()).ok());
    }
    auto offsets = message_offsets(read(root_ / "staging/1.mcap"));
    ASSERT_EQ(offsets.size(), records.size());
    const auto fault_at =
        mcap_fault ? offsets[prefix] + 9 + 22 + 4 : format::kMetaHeaderBytes + prefix * 80 + 40;
    std::uint64_t confirmed = 0;
    unsigned hits = 0;
    const format::CheckedSink::Write fault = [&](int fd, const void* bytes,
                                                 std::size_t count) -> ssize_t {
        if (count > fault_at - confirmed) {
            ++hits;
            if (mode == 0 || mode == 1) {
                errno = mode == 0 ? EIO : ENOSPC;
                return -1;
            }
            if (mode == 3) {
                return 0;
            }
            const auto partial = static_cast<std::size_t>(fault_at - confirmed);
            const auto result = ::write(fd, bytes, partial);
            if (result > 0) {
                confirmed += static_cast<std::uint64_t>(result);
            }
            return result;
        }
        const auto result = ::write(fd, bytes, count);
        if (result > 0) {
            confirmed += static_cast<std::uint64_t>(result);
        }
        return result;
    };
    format::StorageBackend backend(
        mcap_fault ? fault : format::CheckedSink::Write{},
        mcap_fault ? format::CheckedSink::Write{} : fault);
    prepare(backend);
    open(backend, 2);
    const auto outcome = backend.write_batch({records.data(), records.size()}, soon());
    EXPECT_EQ(
        outcome.status.code(), mode == 0   ? StatusCode::Unavailable
                               : mode == 1 ? StatusCode::ResourceExhausted
                                           : StatusCode::DataLoss);
    EXPECT_EQ(outcome.batch_result.confirmed_prefix, prefix);
    EXPECT_EQ(outcome.batch_result.tail_state, RODAQ_STORAGE_TAIL_UNKNOWN);
    EXPECT_EQ(hits, 1);
    const auto usage = backend.segment_usage().value();
    EXPECT_FALSE(backend.write_batch({records.data(), records.size()}, soon()).status.ok());
    EXPECT_FALSE(backend.seal_segment(soon()).ok());
    EXPECT_EQ(backend.segment_usage().value().physical_bytes, usage.physical_bytes);
    EXPECT_EQ(hits, 1);
    EXPECT_TRUE(backend.quiesce(soon()).ok());
}

INSTANTIATE_TEST_SUITE_P(
    CompositeIo, McapCompositeFaultTest,
    testing::Combine(testing::Bool(), testing::Range(0u, 4u), testing::Range(0u, 3u)));

TEST_F(McapStorageTest, ActualSdkLoadsOfficialPluginAndReceivesCallerOwnedSealFiles) {
    fs::copy(RODAQ_MCAP_PACKAGE, root_ / "packages", fs::copy_options::recursive);
    for (const auto& entry : fs::recursive_directory_iterator(root_ / "packages")) {
        fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::owner_exec);
    }
    fs::permissions(root_ / "packages", fs::perms::owner_read | fs::perms::owner_exec);
    plugin::PluginManagerOptions options;
    options.search_paths = {root_ / "packages"};
    options.max_loaded_plugins = 1;
    options.max_instances = 1;
    options.max_config_size = 4096;
    options.shutdown_timeout = 2s;
    auto created = plugin::PluginManager::create(options);
    ASSERT_TRUE(created.ok());
    manager_.emplace(std::move(created.value()));
    auto library = manager_->load("com.rodaq.storage.mcap", "1.0.0");
    ASSERT_TRUE(library.ok()) << library.status().message();
    plugin::ServiceRegistry registry;
    ASSERT_TRUE(registry.freeze().ok());
    auto instance = library.value().create_instance(registry, R"({"format_version":"1.0"})");
    ASSERT_TRUE(instance.ok()) << instance.status().message();
    {
        auto handle = instance.value().query<storage::Interface>();
        ASSERT_TRUE(handle.ok());
        ASSERT_TRUE(handle.value().prepare(envelope(), soon()).ok());
        ASSERT_TRUE(handle.value().prepare_descriptor(descriptor(), soon()).ok());
        const auto path = (root_ / "staging").string();
        storage::SegmentSpec spec{
            {sizeof(storage::SegmentSpec)},
            5,
            {path.data(), static_cast<std::uint32_t>(path.size())}};
        ASSERT_TRUE(handle.value().open_segment(spec, soon()).ok());
        auto input = record();
        ASSERT_TRUE(handle.value().write_batch({&input, 1}, soon()).status.ok());
        auto sealed = handle.value().seal_segment(soon());
        ASSERT_TRUE(sealed.ok());
        EXPECT_EQ(sealed.value().files[0].relative_path, "5.mcap");
        EXPECT_EQ(sealed.value().files[1].relative_path, "5.rodaqmeta");
        ASSERT_TRUE(handle.value().quiesce(soon()).ok());
    }
    ASSERT_TRUE(instance.value().close(soon()).ok());
    EXPECT_TRUE(manager_->shutdown(soon()).ok());
}

TEST_F(McapStorageTest, ExpiredDeadlineAtEntryPreventsAllMutableSideEffects) {
    format::StorageBackend backend;
    ASSERT_TRUE(backend.configure(R"({"format_version":"1.0"})").ok());

    const auto expired = plugin::Deadline::after(0ns).value();
    EXPECT_EQ(backend.prepare(envelope(), expired).status().code(), StatusCode::DeadlineExceeded);
    ASSERT_TRUE(backend.prepare(envelope(), soon()).ok());

    EXPECT_EQ(
        backend.prepare_descriptor(descriptor(), expired).status().code(),
        StatusCode::DeadlineExceeded);
    ASSERT_TRUE(backend.prepare_descriptor(descriptor(), soon()).ok());

    const auto staging = (root_ / "staging").string();
    storage::SegmentSpec spec{
        {sizeof(storage::SegmentSpec)},
        7,
        {staging.data(), static_cast<std::uint32_t>(staging.size())}};
    EXPECT_EQ(backend.open_segment(spec, expired).status().code(), StatusCode::DeadlineExceeded);
    EXPECT_FALSE(fs::exists(root_ / "staging/7.mcap"));
    EXPECT_FALSE(fs::exists(root_ / "staging/7.rodaqmeta"));
    ASSERT_TRUE(backend.open_segment(spec, soon()).ok());

    auto item = record();
    const auto write_res = backend.write_batch({&item, 1}, expired);
    EXPECT_EQ(write_res.status.code(), StatusCode::DeadlineExceeded);
    EXPECT_EQ(write_res.batch_result.confirmed_prefix, 0u);
    EXPECT_EQ(write_res.batch_result.tail_state, RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN);
    ASSERT_TRUE(backend.write_batch({&item, 1}, soon()).status.ok());

    EXPECT_EQ(backend.seal_segment(expired).status().code(), StatusCode::DeadlineExceeded);
    ASSERT_TRUE(backend.seal_segment(soon()).ok());

    EXPECT_EQ(backend.quiesce(expired).status().code(), StatusCode::DeadlineExceeded);
    ASSERT_TRUE(backend.quiesce(soon()).ok());
}

TEST_F(McapStorageTest, MidBatchAndBetweenPairedWritesDeadlineReportsExactPrefixAndUnknownTail) {
    // Part A: deadline expires after record 0 completes both MCAP & sidecar,
    // before record 1 starts -> confirmed_prefix=1, DEFINITELY_UNWRITTEN, still Open.
    {
        int meta_writes = 0;
        format::CheckedSink::Write slow_meta = [&](int fd, const void* bytes,
                                                   std::size_t count) -> ssize_t {
            ++meta_writes;
            if (meta_writes == 2) {  // 1st is header, 2nd is record 0 sidecar
                std::this_thread::sleep_for(30ms);
            }
            return ::write(fd, bytes, count);
        };
        format::StorageBackend backend({}, slow_meta);
        prepare(backend);
        open(backend, 11);
        std::array<storage::Record, 2> records{record(), record()};
        records[1].acceptance_sequence++;
        records[1].receive_monotonic++;
        const auto res = backend.write_batch(
            {records.data(), records.size()}, plugin::Deadline::after(15ms).value());
        EXPECT_EQ(res.status.code(), StatusCode::DeadlineExceeded);
        EXPECT_EQ(res.batch_result.confirmed_prefix, 1u);
        EXPECT_EQ(res.batch_result.tail_state, RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN);
        // Segment stayed clean and sealable.
        ASSERT_TRUE(backend.seal_segment(soon()).ok());
        ASSERT_TRUE(backend.quiesce(soon()).ok());
    }

    // Part B: deadline expires AFTER record 0 MCAP write, BEFORE record 0 sidecar write ->
    // sidecar write is NOT started, confirmed_prefix=0, tail_state=UNKNOWN, fail-closed.
    {
        int mcap_writes = 0;
        int meta_entry_writes = 0;
        format::CheckedSink::Write slow_mcap = [&](int fd, const void* bytes,
                                                   std::size_t count) -> ssize_t {
            const auto out = ::write(fd, bytes, count);
            ++mcap_writes;
            if (mcap_writes >= 4) {  // trigger during message write
                std::this_thread::sleep_for(30ms);
            }
            return out;
        };
        format::CheckedSink::Write count_meta = [&](int fd, const void* bytes,
                                                    std::size_t count) -> ssize_t {
            ++meta_entry_writes;
            return ::write(fd, bytes, count);
        };
        format::StorageBackend backend(slow_mcap, count_meta);
        prepare(backend);
        open(backend, 12);
        const int meta_before = meta_entry_writes;
        auto item = record();
        const auto res = backend.write_batch({&item, 1}, plugin::Deadline::after(15ms).value());
        EXPECT_EQ(res.status.code(), StatusCode::DeadlineExceeded);
        EXPECT_EQ(res.batch_result.confirmed_prefix, 0u);
        EXPECT_EQ(res.batch_result.tail_state, RODAQ_STORAGE_TAIL_UNKNOWN);
        EXPECT_EQ(meta_entry_writes, meta_before);
        EXPECT_FALSE(backend.seal_segment(soon()).ok());
        ASSERT_TRUE(backend.quiesce(soon()).ok());
    }
}

}  // namespace
