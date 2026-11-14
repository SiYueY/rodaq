#include <rodaq/plugin/abi.hpp>
#include <rodaq/plugin/deadline.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

namespace {

using rodaq::plugin::Deadline;
namespace abi = rodaq::plugin::abi;

TEST(AbiErrorBuffer, RejectsChangedBackingBeforeDereference) {
    std::array<std::uint8_t, 8> backing{};
    const abi::BufferSnapshot expected{backing.data(), backing.size()};
    rodaq_error_t error{{backing.data(), backing.size(), 0}};
    EXPECT_TRUE(abi::valid_error(RODAQ_STATUS_OK, error, expected));

    error.message.data = reinterpret_cast<std::uint8_t*>(1);
    error.message.size = 1;
    EXPECT_FALSE(abi::valid_error(RODAQ_STATUS_INTERNAL, error, expected));
    EXPECT_EQ(
        abi::status_from_abi(RODAQ_STATUS_INVALID_ARGUMENT, error, expected).code(),
        rodaq::StatusCode::InvalidArgument);
    EXPECT_EQ(
        abi::status_from_abi(RODAQ_STATUS_OK, error, expected).code(), rodaq::StatusCode::Internal);

    error.message = {backing.data(), backing.size() - 1, 0};
    EXPECT_FALSE(abi::valid_error(RODAQ_STATUS_OK, error, expected));
    error.message = {backing.data(), backing.size(), backing.size() + 1};
    EXPECT_FALSE(abi::valid_error(RODAQ_STATUS_INTERNAL, error, expected));
}

TEST(AbiErrorBuffer, EmptyCapacityAndKnownPrimaryStatus) {
    const abi::BufferSnapshot expected{nullptr, 0};
    rodaq_error_t error{};
    EXPECT_TRUE(abi::valid_error(RODAQ_STATUS_OK, error, expected));
    EXPECT_TRUE(abi::status_from_abi(RODAQ_STATUS_OK, error, expected).ok());
    EXPECT_EQ(abi::status_from_abi(99, error, expected).code(), rodaq::StatusCode::Internal);
    error.message = {nullptr, 1, 0};
    EXPECT_FALSE(abi::valid_error(RODAQ_STATUS_OK, error, expected));
    EXPECT_EQ(
        abi::status_from_abi(RODAQ_STATUS_UNAVAILABLE, error, expected).code(),
        rodaq::StatusCode::Unavailable);
}

TEST(AbiErrorBuffer, TruncatesOnlyCompleteUtf8CodePoints) {
    const std::string text = "A\xc2\xa2\xe2\x82\xac\xf0\x90\x8d\x88";
    constexpr std::array<std::size_t, 11> kPrefix{0, 1, 1, 3, 3, 3, 6, 6, 6, 6, 10};
    std::array<std::uint8_t, 12> backing{};
    for (std::size_t capacity = 0; capacity <= text.size(); ++capacity) {
        backing.fill(0xff);
        rodaq_error_t error{{backing.data(), static_cast<std::uint32_t>(capacity), 99}};
        abi::write_error(&error, text);
        EXPECT_EQ(error.message.data, backing.data());
        EXPECT_EQ(error.message.capacity, capacity);
        EXPECT_EQ(error.message.size, kPrefix[capacity]);
        EXPECT_EQ(backing[capacity], 0xff);
    }
    abi::write_error(nullptr, text);
}

TEST(AbiErrorBuffer, InvalidUtf8AndOkDiagnosticFailClosed) {
    for (const std::string_view text :
         {"\x80", "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\xf5\x80\x80\x80"}) {
        EXPECT_FALSE(abi::valid_string({text.data(), static_cast<std::uint32_t>(text.size())}));
    }
    EXPECT_TRUE(abi::valid_string({nullptr, 0}));
    EXPECT_FALSE(abi::valid_string({nullptr, 1}));
    std::array<std::uint8_t, 3> backing{'o', 'k', 0};
    rodaq_error_t error{{backing.data(), backing.size(), 2}};
    const abi::BufferSnapshot expected{backing.data(), backing.size()};
    EXPECT_EQ(
        abi::status_from_abi(RODAQ_STATUS_OK, error, expected).code(), rodaq::StatusCode::Internal);
    const auto failure = abi::status_from_abi(RODAQ_STATUS_DATA_LOSS, error, expected);
    EXPECT_EQ(failure.code(), rodaq::StatusCode::DataLoss);
    EXPECT_EQ(failure.message(), "ok");
}

struct MappingDeleter {
    std::size_t size;

    void operator()(char* memory) const noexcept { munmap(memory, size); }
};

TEST(AbiVtable, NeverReadsHeaderOrTailBeyondPublishedRange) {
    const auto page_size = sysconf(_SC_PAGESIZE);
    ASSERT_GT(page_size, 0);
    const auto size = static_cast<std::size_t>(page_size);
    auto* memory = static_cast<char*>(
        mmap(nullptr, size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    ASSERT_NE(memory, MAP_FAILED);
    std::unique_ptr<char, MappingDeleter> mapping(memory, MappingDeleter{size * 2});
    ASSERT_EQ(mprotect(memory + size, size, PROT_NONE), 0);
    int context = 0;
    rodaq_interface_reference_t reference{};
    reference.context = reinterpret_cast<rodaq_interface_context_t*>(&context);
    reference.vtable = reinterpret_cast<const rodaq_interface_vtable_t*>(memory + size);
    reference.vtable_size = 0;
    EXPECT_EQ(abi::readable_vtable_size<rodaq_plugin_instance_vtable_t>(reference), 0);
    reference.vtable_size = sizeof(rodaq_struct_header_t) - 1;
    EXPECT_EQ(abi::readable_vtable_size<rodaq_plugin_instance_vtable_t>(reference), 0);

    auto* prefix = memory + size - alignof(rodaq_plugin_instance_vtable_t);
    const rodaq_struct_header_t header{std::numeric_limits<std::uint32_t>::max()};
    std::memcpy(prefix, &header, sizeof(header));
    reference.vtable = reinterpret_cast<const rodaq_interface_vtable_t*>(prefix);
    reference.vtable_size = sizeof(header);
    EXPECT_EQ(abi::readable_vtable_size<rodaq_plugin_instance_vtable_t>(reference), sizeof(header));
    EXPECT_FALSE(abi::readable_slot(
        sizeof(header), offsetof(rodaq_plugin_instance_vtable_t, query),
        sizeof(((rodaq_plugin_instance_vtable_t*)nullptr)->query)));
    EXPECT_FALSE(abi::readable_slot(8, std::numeric_limits<std::size_t>::max(), 8));
}

TEST(PluginDeadline, AbsoluteClockAndFiniteSaturation) {
    EXPECT_TRUE(Deadline::infinite().is_infinite());
    EXPECT_EQ(Deadline::infinite().native().monotonic_deadline, RODAQ_DEADLINE_INFINITE);
    EXPECT_EQ(Deadline::from_native({-1}).status().code(), rodaq::StatusCode::InvalidArgument);
    EXPECT_EQ(
        Deadline::after(std::chrono::nanoseconds(-1)).status().code(),
        rodaq::StatusCode::InvalidArgument);
    const auto expired = Deadline::from_native({0});
    ASSERT_TRUE(expired.ok());
    EXPECT_EQ(expired.value().remaining().value().count(), 0);
    const auto saturated = Deadline::after(std::chrono::nanoseconds::max());
    ASSERT_TRUE(saturated.ok());
    EXPECT_FALSE(saturated.value().is_infinite());
    EXPECT_EQ(
        saturated.value().native().monotonic_deadline,
        std::numeric_limits<std::int64_t>::max() - 1);
}

}  // namespace
