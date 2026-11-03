#include "runtime/interop_projection_registry.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using rodaq::runtime::InteropProjectionRegistry;
using rodaq::runtime::ProjectionLimits;
using rodaq::runtime::ProjectionView;

rodaq::runtime::ProjectionLimits limits(
    std::size_t id_bytes = 64, std::size_t count = 8, std::size_t descriptor_bytes = 256,
    std::size_t activation_bytes = 1024) {
    return ProjectionLimits{
        id_bytes,
        count,
        descriptor_bytes,
        activation_bytes,
    };
}

rodaq_stream_descriptor_handle descriptor(std::uint64_t value) noexcept {
    return rodaq_stream_descriptor_handle{value};
}

const std::uint8_t* bytes(std::string_view value) noexcept {
    return reinterpret_cast<const std::uint8_t*>(value.data());
}

template <typename T>
T take(rodaq::Result<T> result) {
    if (!result.has_value()) {
        throw std::runtime_error("Expected successful Runtime operation");
    }
    return std::move(result.value());
}

}  // namespace

TEST(InteropProjectionRegistryTest, RegistersAndQueriesImmutableOwnedBytes) {
    InteropProjectionRegistry registry(limits());
    std::string projection_id = "rodaq.test.projection.v1";
    std::string payload = "payload";

    ASSERT_TRUE(registry.Register(descriptor(7), projection_id, bytes(payload), payload.size())
                    .has_value());
    projection_id.assign("mutated");
    std::memset(payload.data(), 'x', payload.size());

    const ProjectionView view = take(registry.Get(descriptor(7), "rodaq.test.projection.v1"));
    EXPECT_EQ("rodaq.test.projection.v1", view.projection_id);
    ASSERT_EQ(7U, view.payload_size);
    EXPECT_EQ(0, std::memcmp(view.payload, "payload", view.payload_size));
    EXPECT_EQ(rodaq::ErrorCode::NotFound, registry.Get(descriptor(7), "missing").error().code());
}

TEST(InteropProjectionRegistryTest, AppliesIdempotenceAndConflictSemantics) {
    InteropProjectionRegistry registry(limits());
    ASSERT_TRUE(registry.Register(descriptor(1), "id", bytes("abc"), 3).has_value());
    ASSERT_TRUE(registry.Register(descriptor(1), "id", bytes("abc"), 3).has_value());

    EXPECT_EQ(
        rodaq::ErrorCode::Conflict,
        registry.Register(descriptor(1), "id", bytes("abd"), 3).error().code());
}

TEST(InteropProjectionRegistryTest, KeysByDescriptorAndProjectionId) {
    InteropProjectionRegistry registry(limits());
    ASSERT_TRUE(registry.Register(descriptor(1), "one", bytes("a"), 1).has_value());
    ASSERT_TRUE(registry.Register(descriptor(1), "two", bytes("b"), 1).has_value());
    ASSERT_TRUE(registry.Register(descriptor(2), "one", bytes("c"), 1).has_value());

    EXPECT_EQ(
        "a",
        std::string_view(
            reinterpret_cast<const char*>(take(registry.Get(descriptor(1), "one")).payload), 1));
    EXPECT_EQ(
        "b",
        std::string_view(
            reinterpret_cast<const char*>(take(registry.Get(descriptor(1), "two")).payload), 1));
    EXPECT_EQ(
        "c",
        std::string_view(
            reinterpret_cast<const char*>(take(registry.Get(descriptor(2), "one")).payload), 1));
}

TEST(InteropProjectionRegistryTest, RejectsInvalidKeysAndPayloads) {
    InteropProjectionRegistry registry(limits(4, 2, 8, 16));
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument,
        registry.Register(descriptor(0), "id", nullptr, 0).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument,
        registry.Register(descriptor(1), "", nullptr, 0).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument,
        registry.Register(descriptor(1), std::string_view("\xFF", 1), nullptr, 0).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::ResourceExhausted,
        registry.Register(descriptor(1), "toolong", nullptr, 0).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument,
        registry.Register(descriptor(1), "id", nullptr, 1).error().code());

    ASSERT_TRUE(registry.Register(descriptor(1), "zero", nullptr, 0).has_value());
    const ProjectionView zero = take(registry.Get(descriptor(1), "zero"));
    EXPECT_EQ(nullptr, zero.payload);
    EXPECT_EQ(0U, zero.payload_size);
}

TEST(InteropProjectionRegistryTest, EnforcesAllBoundsAtomically) {
    InteropProjectionRegistry registry(limits(64, 2, 4, 5));
    ASSERT_TRUE(registry.Register(descriptor(1), "one", bytes("abc"), 3).has_value());
    ASSERT_TRUE(registry.Register(descriptor(1), "two", bytes("x"), 1).has_value());
    EXPECT_EQ(
        rodaq::ErrorCode::ResourceExhausted,
        registry.Register(descriptor(1), "three", bytes("z"), 1).error().code());

    EXPECT_EQ(
        rodaq::ErrorCode::ResourceExhausted,
        registry.Register(descriptor(2), "big", bytes("abc"), 3).error().code());
    ASSERT_TRUE(registry.Register(descriptor(2), "small", bytes("d"), 1).has_value());
    EXPECT_EQ(rodaq::ErrorCode::NotFound, registry.Get(descriptor(2), "big").error().code());
    EXPECT_EQ(1U, take(registry.Get(descriptor(2), "small")).payload_size);
}

TEST(InteropProjectionRegistryTest, ReturnsStableViewsAcrossLaterRegistration) {
    InteropProjectionRegistry registry(limits(64, 16));
    ASSERT_TRUE(registry.Register(descriptor(1), "stable", bytes("value"), 5).has_value());
    const ProjectionView before = take(registry.Get(descriptor(1), "stable"));

    for (int index = 0; index < 8; ++index) {
        const std::string id = "entry." + std::to_string(index);
        ASSERT_TRUE(registry.Register(descriptor(1), id, bytes("x"), 1).has_value());
    }

    EXPECT_EQ("stable", before.projection_id);
    EXPECT_EQ(0, std::memcmp(before.payload, "value", 5));
}

TEST(InteropProjectionRegistryTest, AbiAdapterUsesCallerOwnedAppendableOutput) {
    InteropProjectionRegistry registry(limits());
    const rodaq_interface interface = registry.AbiInterface();
    const auto* vtable = static_cast<const rodaq_runtime_projection_v1*>(interface.vtable);
    ASSERT_NE(nullptr, vtable);
    EXPECT_EQ(RODAQ_RUNTIME_PROJECTION_PROTOCOL_VERSION, vtable->protocol_version);
    EXPECT_EQ(RODAQ_RUNTIME_PROJECTION_V1_MIN_STRUCT_SIZE, vtable->struct_size);

    const char id[] = "rodaq.test.abi.v1";
    const char payload[] = "abi";
    rodaq_projection_registration registration{};
    registration.struct_size = RODAQ_PROJECTION_REGISTRATION_MIN_STRUCT_SIZE;
    registration.descriptor = descriptor(9);
    registration.projection_id = rodaq_string_view{id, sizeof(id) - 1};
    registration.payload = rodaq_bytes_view{bytes(payload), sizeof(payload) - 1};
    rodaq_error error{};
    ASSERT_EQ(
        RODAQ_STATUS_OK, vtable->register_projection(interface.context, &registration, &error));

    rodaq_projection_view output{};
    output.struct_size = RODAQ_PROJECTION_VIEW_MIN_STRUCT_SIZE;
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        vtable->get_projection(
            interface.context, descriptor(9), registration.projection_id, &output, &error));
    EXPECT_EQ(sizeof(rodaq_projection_view), output.struct_size);
    EXPECT_EQ(0U, output.reserved);
    EXPECT_EQ(9U, output.descriptor.value);
    EXPECT_EQ(3U, output.payload.size);
    EXPECT_EQ(0, std::memcmp(output.payload.data, payload, sizeof(payload) - 1));

    output.struct_size = RODAQ_PROJECTION_VIEW_MIN_STRUCT_SIZE - 1;
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        vtable->get_projection(
            interface.context, descriptor(9), registration.projection_id, &output, &error));
    EXPECT_EQ(0U, output.struct_size);

    registration.reserved = 1;
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        vtable->register_projection(interface.context, &registration, &error));
    EXPECT_NE(0U, error.message_size);

    struct GuardedOutput {
        rodaq_projection_view value;
        std::uint8_t canary[32];
    } guarded{};
    std::memset(guarded.canary, 0xA5, sizeof(guarded.canary));
    guarded.value.struct_size = RODAQ_PROJECTION_VIEW_MIN_STRUCT_SIZE;
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        vtable->get_projection(
            interface.context, descriptor(9), registration.projection_id, &guarded.value, &error));
    for (std::uint8_t byte : guarded.canary) {
        EXPECT_EQ(0xA5, byte);
    }
}

TEST(InteropProjectionRegistryTest, SupportsConcurrentReadersWithSerializedWriter) {
    InteropProjectionRegistry registry(limits(64, 16, 256, 256));
    ASSERT_TRUE(registry.Register(descriptor(1), "base", bytes("stable"), 6).has_value());
    const ProjectionView baseline = take(registry.Get(descriptor(1), "base"));

    std::atomic<bool> failed{false};
    std::thread writer([&registry] {
        for (int index = 0; index < 12; ++index) {
            const std::string id = "writer." + std::to_string(index);
            if (!registry.Register(descriptor(2), id, bytes("x"), 1).has_value()) {
                return;
            }
        }
    });
    std::vector<std::thread> readers;
    for (int reader_index = 0; reader_index < 4; ++reader_index) {
        readers.emplace_back([&registry, &baseline, &failed] {
            for (int iteration = 0; iteration < 500; ++iteration) {
                const auto current = registry.Get(descriptor(1), "base");
                if (!current.has_value() || current.value().projection_id != "base" ||
                    current.value().payload_size != 6 ||
                    std::memcmp(current.value().payload, "stable", 6) != 0) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }
    writer.join();
    for (std::thread& reader : readers) {
        reader.join();
    }
    EXPECT_FALSE(failed.load(std::memory_order_acquire));
    EXPECT_EQ("base", baseline.projection_id);
}
