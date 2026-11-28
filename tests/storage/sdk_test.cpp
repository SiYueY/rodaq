#include <rodaq/storage/provider.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>

namespace {

namespace storage = rodaq::storage;
using rodaq::plugin::Deadline;

struct Backend {
    rodaq::Result<void> prepare(const storage::PrepareConfig&, Deadline) {
        ++calls;
        return {};
    }

    rodaq::Result<void> prepare_descriptor(const storage::StreamDescriptor&, Deadline) {
        ++calls;
        return {};
    }

    rodaq::Result<void> open_segment(const storage::SegmentSpec&, Deadline) {
        ++calls;
        return {};
    }

    storage::WriteOutcome write_batch(storage::Batch batch, Deadline) {
        ++calls;
        if (throw_error) {
            throw std::runtime_error("failure after write entry");
        }
        return {
            rodaq::Status(rodaq::StatusCode::Unavailable, "partial"),
            {{sizeof(storage::BatchResult)}, batch.count - 1, RODAQ_STORAGE_TAIL_UNKNOWN}};
    }

    rodaq::Result<storage::SegmentUsage> segment_usage() const {
        return storage::SegmentUsage{{sizeof(storage::SegmentUsage)}, 128, 1, 0};
    }

    rodaq::Result<storage::SegmentSealResult> seal_segment(Deadline) {
        ++calls;
        return storage::SegmentSealResult{{{"one.mcap", 3}}, 3};
    }

    rodaq::Result<void> quiesce(Deadline) {
        ++calls;
        return {};
    }

    std::uint32_t calls{0};
    bool throw_error{false};
};

storage::PrepareConfig envelope() {
    return {{sizeof(storage::PrepareConfig)}, 4, 8, 2, 2, 100, 1024, 4096, 2, 64};
}

storage::Record make_record() {
    storage::Record result{};
    result.header.struct_size = sizeof(result);
    result.stream_id = 1;
    result.descriptor_id = 1;
    result.recording_time = 1000;
    return result;
}

TEST(StorageSdk, PrepareAndSegmentUsagePreserveCallerHeaderAndGuardBytes) {
    Backend backend;
    auto* context = reinterpret_cast<rodaq_interface_context_t*>(&backend);
    const auto config = envelope();
    EXPECT_EQ(
        storage::Provider<Backend>::prepare(context, &config, {RODAQ_DEADLINE_INFINITE}, nullptr),
        RODAQ_STATUS_OK);
    EXPECT_EQ(backend.calls, 1U);

    struct Guarded {
        storage::SegmentUsage usage;
        std::array<std::uint8_t, 16> guard;
    } output{};
    output.guard.fill(0xa5);
    output.usage.header.struct_size = sizeof(output.usage);
    EXPECT_EQ(
        storage::Provider<Backend>::segment_usage(context, &output.usage, nullptr),
        RODAQ_STATUS_OK);
    EXPECT_EQ(output.usage.header.struct_size, sizeof(output.usage));
    EXPECT_EQ(output.usage.physical_bytes, 128ULL);
    EXPECT_EQ(output.usage.file_count, 1U);
    for (auto byte : output.guard) {
        EXPECT_EQ(byte, 0xa5);
    }

    output.usage.header.struct_size = 0;
    output.usage.physical_bytes = 999;
    EXPECT_EQ(
        storage::Provider<Backend>::segment_usage(context, &output.usage, nullptr),
        RODAQ_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(output.usage.physical_bytes, 999ULL);
}

TEST(StorageSdk, ValidationFailureIsUnwrittenButEnteredExceptionIsUnknown) {
    Backend backend;
    auto value = make_record();
    storage::BatchResult output{{sizeof(output)}, 99, RODAQ_STORAGE_TAIL_NONE};
    value.stream_id = 0;
    auto* context = reinterpret_cast<rodaq_interface_context_t*>(&backend);
    EXPECT_EQ(
        storage::Provider<Backend>::write_batch(
            context, &value, 1, {RODAQ_DEADLINE_INFINITE}, &output, nullptr),
        RODAQ_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(output.confirmed_prefix, 0U);
    EXPECT_EQ(output.tail_state, RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN);
    EXPECT_EQ(backend.calls, 0U);

    value = make_record();
    backend.throw_error = true;
    EXPECT_EQ(
        storage::Provider<Backend>::write_batch(
            context, &value, 1, {RODAQ_DEADLINE_INFINITE}, &output, nullptr),
        RODAQ_STATUS_INTERNAL);
    EXPECT_EQ(output.confirmed_prefix, 0U);
    EXPECT_EQ(output.tail_state, RODAQ_STORAGE_TAIL_UNKNOWN);
    EXPECT_EQ(backend.calls, 1U);
}

TEST(StorageSdk, SealValidatesAllBuffersBeforeBackendAndPreservesUnusedIdentity) {
    Backend backend;
    auto* context = reinterpret_cast<rodaq_interface_context_t*>(&backend);
    std::array<char, 16> path{};
    std::array<char, 16> unused{};
    std::array<rodaq_sealed_file_t, 2> files{
        {{{path.data(), path.size(), 0}, 0}, {{nullptr, 0, 0}, 0}}};
    rodaq_segment_seal_result_t output{{sizeof(output)}, files.data(), files.size(), 0, 0};
    EXPECT_EQ(
        storage::Provider<Backend>::seal_segment(
            context, {RODAQ_DEADLINE_INFINITE}, &output, nullptr),
        RODAQ_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(backend.calls, 0U);
    files[1].relative_path = {unused.data(), unused.size(), 0};
    EXPECT_EQ(
        storage::Provider<Backend>::seal_segment(
            context, {RODAQ_DEADLINE_INFINITE}, &output, nullptr),
        RODAQ_STATUS_OK);
    EXPECT_EQ(backend.calls, 1U);
    EXPECT_EQ(output.files, files.data());
    EXPECT_EQ(output.file_capacity, files.size());
    EXPECT_EQ(output.file_count, 1U);
    EXPECT_EQ(output.total_physical_bytes, 3ULL);
    EXPECT_EQ(files[1].relative_path.data, unused.data());
    EXPECT_EQ(files[1].relative_path.capacity, unused.size());
    EXPECT_EQ(files[1].relative_path.size, 0U);
    EXPECT_EQ(files[1].physical_size, 0ULL);
    EXPECT_STREQ(path.data(), "one.mcap");
}

TEST(StorageValidation, FullFailedPrefixAndPartialFailureAreDifferentValidOutcomes) {
    storage::BatchResult result{{sizeof(result)}, 2, RODAQ_STORAGE_TAIL_NONE};
    EXPECT_TRUE(storage::abi::valid_outcome(RODAQ_STATUS_UNAVAILABLE, result, 2));
    result.confirmed_prefix = 1;
    result.tail_state = RODAQ_STORAGE_TAIL_UNKNOWN;
    EXPECT_TRUE(storage::abi::valid_outcome(RODAQ_STATUS_UNAVAILABLE, result, 2));
    EXPECT_FALSE(storage::abi::valid_outcome(RODAQ_STATUS_OK, result, 2));
    EXPECT_FALSE(storage::abi::valid_outcome(99, result, 2));
    result.confirmed_prefix = 3;
    EXPECT_FALSE(storage::abi::valid_outcome(RODAQ_STATUS_UNAVAILABLE, result, 2));
}

}  // namespace
