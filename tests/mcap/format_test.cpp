#include "checked_sink.hpp"
#include "rodaqmeta.hpp"

#include <gtest/gtest.h>
#include <mcap/reader.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

namespace format = rodaq::mcap_storage;
using rodaq::StatusCode;

std::string fixture(const char* name) {
    std::ifstream file(std::filesystem::path(RODAQ_MCAP_FIXTURES) / name);
    std::string hex;
    file >> hex;
    std::string bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

template <std::size_t N>
std::string bytes(const std::array<std::byte, N>& input, std::size_t size = N) {
    return {reinterpret_cast<const char*>(input.data()), size};
}

rodaq::storage::Record record() {
    rodaq::storage::Record result{};
    result.header.struct_size = sizeof(result);
    result.stream_id = 1;
    result.acceptance_sequence = 2;
    result.descriptor_id = 3;
    result.system_boot_id = 4;
    result.clock_epoch_id = 5;
    result.receive_monotonic = 6;
    result.receive_realtime = -7;
    return result;
}

TEST(RodaqMeta, IndependentPythonStructGoldensFreezeHeaderMinimalEntryAndFooter) {
    EXPECT_EQ(bytes(format::encode_meta_header()), fixture("header.hex"));
    const auto encoded = format::encode_meta_entry(record(), 0);
    ASSERT_TRUE(encoded.ok());
    EXPECT_EQ(encoded.value().size, 80);
    EXPECT_EQ(bytes(encoded.value().bytes, encoded.value().size), fixture("minimal.hex"));
    const auto footer = format::encode_meta_footer({1, 104});
    ASSERT_TRUE(footer.ok());
    EXPECT_EQ(bytes(footer.value()), fixture("footer.hex"));

    const auto decoded = format::decode_meta_entry(fixture("minimal.hex"), 0);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().ordinal, 0);
    EXPECT_EQ(decoded.value().record.receive_realtime, -7);
    EXPECT_EQ(decoded.value().record.acceptance_sequence, 2);
    EXPECT_EQ(decoded.value().record.descriptor_id, 3);
    EXPECT_EQ(decoded.value().record.system_boot_id, 4);
    EXPECT_EQ(decoded.value().record.clock_epoch_id, 5);
}

TEST(RodaqMeta, MaximumPresenceAndTimestampEncodingHasExact324ByteBound) {
    auto input = record();
    input.has_source_id = 1;
    input.has_source_session_id = 1;
    input.has_source_sequence = 1;
    input.source_id.kind = RODAQ_SOURCE_ID_ROS2_GID;
    input.source_id.size = 24;
    for (unsigned i = 0; i < 24; ++i) {
        input.source_id.data[i] = static_cast<std::uint8_t>(i + 1);
    }
    input.source_session_id = 8;
    input.source_sequence = 10;
    input.sequence_scope = RODAQ_SEQUENCE_SCOPE_SOURCE_SESSION;
    input.timestamp_count = 6;
    input.delivery_flags = RODAQ_RECORD_DELIVERY_RETAINED;
    for (unsigned i = 0; i < 6; ++i) {
        input.timestamps[i] = {
            RODAQ_TIMESTAMP_ROLE_SOURCE_PUBLICATION,
            RODAQ_CLOCK_DOMAIN_UNIX_EPOCH,
            0,
            static_cast<std::int64_t>(i) - 7,
            0,
            0};
    }
    const auto encoded = format::encode_meta_entry(input, 9);
    ASSERT_TRUE(encoded.ok());
    EXPECT_EQ(encoded.value().size, format::kMetaMaxEntryBytes);
    EXPECT_EQ(bytes(encoded.value().bytes), fixture("maximal.hex"));
    const auto decoded = format::decode_meta_entry(fixture("maximal.hex"), 0);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().ordinal, 9);
    EXPECT_EQ(decoded.value().record.timestamp_count, 6);
    EXPECT_EQ(decoded.value().record.source_id.data[23], 24);
    EXPECT_EQ(decoded.value().record.source_session_id, 8);
    EXPECT_EQ(decoded.value().record.source_sequence, 10);
    EXPECT_EQ(decoded.value().record.delivery_flags, RODAQ_RECORD_DELIVERY_RETAINED);
}

TEST(RodaqMeta, TruncationInvalidPresenceAndUnknownMajorFailClosed) {
    const auto full = fixture("maximal.hex");
    ASSERT_EQ(full.size(), 324);
    for (std::size_t size = 0; size < full.size(); ++size) {
        EXPECT_FALSE(format::decode_meta_entry(std::string_view(full).substr(0, size), 0).ok())
            << size;
    }
    auto invalid = full;
    invalid[4] = 8;
    EXPECT_FALSE(format::decode_meta_entry(invalid, 1).ok());
    invalid[4] = 6;  // Session/sequence without SourceId.
    EXPECT_FALSE(format::decode_meta_entry(invalid, 0).ok());
    invalid = full;
    invalid[101] = 1;  // SourceId reserved byte.
    EXPECT_FALSE(format::decode_meta_entry(invalid, 0).ok());
    auto header = fixture("header.hex");
    header[8] = 2;
    EXPECT_FALSE(format::decode_meta_header(header).ok());
    header = fixture("header.hex");
    header[16] = 1;
    EXPECT_FALSE(format::decode_meta_header(header).ok());
}

TEST(RodaqMeta, MinorEvolutionSkipsOnlyDeclaredAppendOnlyTails) {
    auto header = fixture("header.hex");
    header[10] = 1;
    header[12] = 28;
    header += "tail";
    auto decoded_header = format::decode_meta_header(header);
    ASSERT_TRUE(decoded_header.ok());
    EXPECT_EQ(decoded_header.value().size, 28);
    auto entry = fixture("minimal.hex");
    entry[0] = 84;
    entry += "tail";
    EXPECT_FALSE(format::decode_meta_entry(entry, 0).ok());
    auto decoded = format::decode_meta_entry(entry, decoded_header.value().minor);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().size, 84);
    EXPECT_EQ(decoded.value().record.receive_realtime, -7);
    entry[4] = 8;
    EXPECT_FALSE(format::decode_meta_entry(entry, 1).ok());
}

TEST(RodaqMeta, ExactSignedTimeAndFooterOffsetsCannotWrap) {
    for (const auto value :
         {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
        auto input = record();
        input.receive_realtime = value;
        auto encoded = format::encode_meta_entry(input, 0).value();
        auto decoded = format::decode_meta_entry(bytes(encoded.bytes, encoded.size), 0);
        ASSERT_TRUE(decoded.ok());
        EXPECT_EQ(decoded.value().record.receive_realtime, value);
    }
    EXPECT_FALSE(format::encode_meta_footer({1, std::numeric_limits<std::uint64_t>::max()}).ok());
    auto footer = fixture("footer.hex");
    std::fill(footer.begin() + 16, footer.end(), static_cast<char>(0xff));
    EXPECT_FALSE(format::decode_meta_footer(footer).ok());
    auto valid = format::decode_meta_footer(fixture("footer.hex"));
    ASSERT_TRUE(valid.ok());
    EXPECT_EQ(valid.value().entry_count, 1);
    EXPECT_EQ(valid.value().bytes_before_footer, 104);
    footer = fixture("footer.hex") + "extra";
    EXPECT_FALSE(format::decode_meta_footer(footer).ok());
}

class CheckedSinkTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> name{};
        const std::string prefix = "/tmp/rodaq-mcap-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), name.begin());
        const auto* created = mkdtemp(name.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        directory_ = ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        ASSERT_GE(directory_, 0);
    }

    void TearDown() override {
        if (directory_ >= 0) {
            ::close(directory_);
        }
        std::filesystem::remove_all(root_);
    }

    std::filesystem::path root_;
    int directory_{-1};
};

TEST_F(CheckedSinkTest, ActualMcapApiWritesChunklessFileReadableByIndependentReader) {
    format::CheckedSink sink;
    ASSERT_TRUE(sink.open(directory_, "probe.mcap").ok());
    mcap::McapWriter writer;
    mcap::McapWriterOptions options("ros2");
    options.noChunking = true;
    options.noSummary = true;
    options.noMessageIndex = true;
    options.noChunkCRC = true;
    options.compression = mcap::Compression::None;
    writer.open(sink, options);
    mcap::Schema schema("std_msgs/msg/String", "ros2msg", "string data\n");
    writer.addSchema(schema);
    mcap::Channel channel("/probe", "cdr", schema.id);
    writer.addChannel(channel);
    const std::array<std::byte, 4> payload{};
    mcap::Message message;
    message.channelId = channel.id;
    message.sequence = 0;
    message.logTime = 100;
    message.publishTime = 100;
    message.data = payload.data();
    message.dataSize = payload.size();
    ASSERT_TRUE(writer.write(message).ok());
    writer.close();
    EXPECT_EQ(sink.status(), StatusCode::Ok);
    EXPECT_EQ(sink.size(), sink.confirmed_bytes());
    EXPECT_EQ(std::filesystem::file_size(root_ / "probe.mcap"), sink.confirmed_bytes());
    mcap::McapReader reader;
    ASSERT_TRUE(reader.open((root_ / "probe.mcap").string()).ok());
    unsigned count = 0;
    for (const auto& view : reader.readMessages()) {
        ++count;
        EXPECT_EQ(view.channel->topic, "/probe");
        EXPECT_EQ(view.schema->name, "std_msgs/msg/String");
        EXPECT_EQ(view.message.dataSize, 4);
    }
    EXPECT_EQ(count, 1);
}

TEST_F(CheckedSinkTest, ShortWriteLatchesAndLaterRequestsAdvanceOnlyLogicalPosition) {
    unsigned calls = 0;
    format::CheckedSink sink([&](int fd, const void* data, std::size_t count) {
        ++calls;
        return ::write(fd, data, count / 2);
    });
    ASSERT_TRUE(sink.open(directory_, "short").ok());
    const std::array<std::byte, 8> payload{};
    sink.write(payload.data(), payload.size());
    EXPECT_EQ(sink.status(), StatusCode::DataLoss);
    EXPECT_EQ(sink.confirmed_bytes(), 4);
    sink.write(payload.data(), payload.size());
    EXPECT_EQ(sink.size(), 16);
    EXPECT_EQ(sink.confirmed_bytes(), 4);
    EXPECT_EQ(calls, 1);
    sink.end();
    EXPECT_EQ(std::filesystem::file_size(root_ / "short"), 4);
}

TEST_F(CheckedSinkTest, EintrRetriesAndDiskFullLatchesThePrimaryError) {
    unsigned calls = 0;
    format::CheckedSink sink([&](int fd, const void* data, std::size_t count) -> ssize_t {
        ++calls;
        if (calls == 1) {
            errno = EINTR;
            return -1;
        }
        if (calls == 2) {
            return ::write(fd, data, count);
        }
        errno = ENOSPC;
        return -1;
    });
    ASSERT_TRUE(sink.open(directory_, "full").ok());
    const std::array<std::byte, 4> payload{};
    sink.write(payload.data(), payload.size());
    EXPECT_EQ(sink.status(), StatusCode::Ok);
    EXPECT_EQ(calls, 2);
    sink.write(payload.data(), payload.size());
    EXPECT_EQ(sink.status(), StatusCode::ResourceExhausted);
    sink.end();
    sink.write(payload.data(), payload.size());
    EXPECT_EQ(sink.status(), StatusCode::ResourceExhausted);
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(sink.size(), 12);
    EXPECT_EQ(sink.confirmed_bytes(), 4);
}

TEST_F(CheckedSinkTest, ExistingFilesAndLinksAreNeverReplaced) {
    std::ofstream(root_ / "existing") << "original";
    std::filesystem::create_symlink("existing", root_ / "link");
    format::CheckedSink existing;
    EXPECT_EQ(existing.open(directory_, "existing").status().code(), StatusCode::Conflict);
    format::CheckedSink linked;
    EXPECT_FALSE(linked.open(directory_, "link").ok());
    EXPECT_EQ(std::filesystem::file_size(root_ / "existing"), 8);
}

}  // namespace
