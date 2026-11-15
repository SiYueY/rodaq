#include <rodaq/storage/capacity.hpp>
#include <rodaq/storage/provider.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {

namespace storage = rodaq::storage;
using rodaq::plugin::Deadline;

struct Backend {
    rodaq::Result<storage::CapacityModel> prepare(const storage::PrepareConfig&, Deadline) {
        ++calls;
        return storage::CapacityModel{{sizeof(storage::CapacityModel)}, 3, 2, 4, 1, 1, 2, 8, 16};
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

    rodaq::Result<storage::SegmentSealResult> seal_segment(Deadline) {
        ++calls;
        return storage::SegmentSealResult{{{"one.bin", 3}}, 3};
    }

    std::uint32_t calls{0};
    bool throw_error{false};
};

storage::PrepareConfig envelope() {
    return {{sizeof(storage::PrepareConfig)}, 4, 8, 2, 2, 100, 1024, 4096, 2, 64, 1, 1, 1};
}

storage::Record record() {
    storage::Record result{};
    result.header.struct_size = sizeof(result);
    result.stream_id = 1;
    result.descriptor_id = 1;
    result.acceptance_sequence = 1;
    result.system_boot_id = 1;
    result.clock_epoch_id = 1;
    return result;
}

TEST(StorageSdk, CapacityOutputPreservesCallerHeaderAndGuardBytes) {
    Backend backend;
    struct Guarded {
        storage::CapacityModel model;
        std::array<std::uint8_t, 16> guard;
    } output{};
    output.guard.fill(0xa5);
    output.model.header.struct_size = sizeof(output.model);
    const auto config = envelope();
    EXPECT_EQ(
        storage::Provider<Backend>::prepare(
            reinterpret_cast<rodaq_interface_context_t*>(&backend), &config,
            {RODAQ_DEADLINE_INFINITE}, &output.model, nullptr),
        RODAQ_STATUS_OK);
    EXPECT_EQ(output.model.header.struct_size, sizeof(output.model));
    EXPECT_EQ(output.model.payload_expansion_denominator, 2);
    EXPECT_EQ(output.model.seal_reserve_bytes, 16);
    for (auto byte : output.guard) {
        EXPECT_EQ(byte, 0xa5);
    }
    output.model.header.struct_size = 0;
    output.model.seal_reserve_bytes = 123;
    EXPECT_EQ(
        storage::Provider<Backend>::prepare(
            reinterpret_cast<rodaq_interface_context_t*>(&backend), &config,
            {RODAQ_DEADLINE_INFINITE}, &output.model, nullptr),
        RODAQ_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(output.model.seal_reserve_bytes, 123);
    EXPECT_EQ(backend.calls, 1);
}

TEST(StorageSdk, ValidationFailureIsUnwrittenButEnteredExceptionIsUnknown) {
    Backend backend;
    auto value = record();
    storage::BatchResult output{{sizeof(output)}, 99, RODAQ_STORAGE_TAIL_NONE};
    value.stream_id = 0;
    auto* context = reinterpret_cast<rodaq_interface_context_t*>(&backend);
    EXPECT_EQ(
        storage::Provider<Backend>::write_batch(
            context, &value, 1, {RODAQ_DEADLINE_INFINITE}, &output, nullptr),
        RODAQ_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(output.confirmed_prefix, 0);
    EXPECT_EQ(output.tail_state, RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN);
    EXPECT_EQ(backend.calls, 0);
    value = record();
    backend.throw_error = true;
    EXPECT_EQ(
        storage::Provider<Backend>::write_batch(
            context, &value, 1, {RODAQ_DEADLINE_INFINITE}, &output, nullptr),
        RODAQ_STATUS_INTERNAL);
    EXPECT_EQ(output.confirmed_prefix, 0);
    EXPECT_EQ(output.tail_state, RODAQ_STORAGE_TAIL_UNKNOWN);
    EXPECT_EQ(backend.calls, 1);
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
    EXPECT_EQ(backend.calls, 0);
    files[1].relative_path = {unused.data(), unused.size(), 0};
    EXPECT_EQ(
        storage::Provider<Backend>::seal_segment(
            context, {RODAQ_DEADLINE_INFINITE}, &output, nullptr),
        RODAQ_STATUS_OK);
    EXPECT_EQ(backend.calls, 1);
    EXPECT_EQ(output.files, files.data());
    EXPECT_EQ(output.file_capacity, files.size());
    EXPECT_EQ(output.file_count, 1);
    EXPECT_EQ(output.total_physical_bytes, 3);
    EXPECT_EQ(files[1].relative_path.data, unused.data());
    EXPECT_EQ(files[1].relative_path.capacity, unused.size());
    EXPECT_EQ(files[1].relative_path.size, 0);
    EXPECT_EQ(files[1].physical_size, 0);
    EXPECT_STREQ(path.data(), "one.bin");
}

TEST(StorageCapacity, RoundingAndAllOverflowPathsAreChecked) {
    EXPECT_EQ(storage::expansion_bound(3, 3, 2, 4).value(), 9);
    EXPECT_EQ(storage::expansion_bound(0, 3, 2, 4).value(), 4);
    EXPECT_EQ(
        storage::expansion_bound(10, 1, 0, 0).status().code(), rodaq::StatusCode::InvalidArgument);
    const auto max = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(
        storage::expansion_bound(max, 2, 1, 0).status().code(),
        rodaq::StatusCode::ResourceExhausted);
    EXPECT_EQ(
        storage::expansion_bound(max, 1, 1, 1).status().code(),
        rodaq::StatusCode::ResourceExhausted);
    EXPECT_EQ(storage::expansion_bound(max, 1, max, 0).value(), 1);
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
