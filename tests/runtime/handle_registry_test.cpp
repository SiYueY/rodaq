#include <rodaq/runtime/handle.hpp>
#include <rodaq/runtime/registry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace rodaq {
namespace {

using IntRegistry = Registry<int, StreamHandle>;

TEST(HandleTest, DefaultHandleIsInvalid) {
    const StreamHandle handle;
    EXPECT_FALSE(handle.Valid());
    EXPECT_EQ(handle.Value(), 0U);
}

TEST(HandleTest, RawHandleAndEquality) {
    const auto first = StreamHandle::FromRaw(42);
    const auto second = StreamHandle::FromRaw(42);
    const auto other = StreamHandle::FromRaw(43);

    EXPECT_TRUE(first.Valid());
    EXPECT_EQ(first, second);
    EXPECT_NE(first, other);
    EXPECT_EQ(first.Value(), 42U);
}

TEST(HandleTest, AllocatorIsMonotonicAndDetectsOverflow) {
    HandleAllocator<StreamHandle> allocator(std::numeric_limits<std::uint64_t>::max());

    const auto last = allocator.Allocate();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last.value().Value(), std::numeric_limits<std::uint64_t>::max());

    const auto exhausted = allocator.Allocate();
    ASSERT_FALSE(exhausted.has_value());
    EXPECT_EQ(exhausted.error().code(), ErrorCode::ResourceExhausted);
}

TEST(RegistryTest, InsertFindEraseAndCapacity) {
    auto registry_result = IntRegistry::Create(2);
    ASSERT_TRUE(registry_result.has_value());
    auto registry = std::move(registry_result.value());

    auto first = registry.Insert(10);
    auto second = registry.Insert(20);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_NE(first.value(), second.value());
    EXPECT_EQ(registry.Size(), 2U);
    EXPECT_EQ(registry.Capacity(), 2U);
    ASSERT_NE(registry.Find(first.value()), nullptr);
    EXPECT_EQ(*registry.Find(first.value()), 10);

    const auto full = registry.Insert(30);
    ASSERT_FALSE(full.has_value());
    EXPECT_EQ(full.error().code(), ErrorCode::ResourceExhausted);

    ASSERT_TRUE(registry.Erase(first.value()).has_value());
    EXPECT_EQ(registry.Find(first.value()), nullptr);
    EXPECT_EQ(registry.Size(), 1U);
}

TEST(RegistryTest, EraseRejectsInvalidAndMissingHandles) {
    auto registry_result = IntRegistry::Create(1);
    ASSERT_TRUE(registry_result.has_value());
    auto registry = std::move(registry_result.value());

    const auto invalid = registry.Erase(StreamHandle{});
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);

    auto handle = registry.Insert(1);
    ASSERT_TRUE(handle.has_value());
    ASSERT_TRUE(registry.Erase(handle.value()).has_value());

    const auto double_erase = registry.Erase(handle.value());
    ASSERT_FALSE(double_erase.has_value());
    EXPECT_EQ(double_erase.error().code(), ErrorCode::NotFound);
}

TEST(RegistryTest, StaleHandleCannotResolveReusedSlot) {
    auto registry_result = IntRegistry::Create(1);
    ASSERT_TRUE(registry_result.has_value());
    auto registry = std::move(registry_result.value());

    auto old_handle = registry.Insert(1);
    ASSERT_TRUE(old_handle.has_value());
    ASSERT_TRUE(registry.Erase(old_handle.value()).has_value());

    auto new_handle = registry.Insert(2);
    ASSERT_TRUE(new_handle.has_value());
    EXPECT_NE(old_handle.value(), new_handle.value());
    EXPECT_EQ(registry.Find(old_handle.value()), nullptr);
    ASSERT_NE(registry.Find(new_handle.value()), nullptr);
    EXPECT_EQ(*registry.Find(new_handle.value()), 2);
}

TEST(RegistryTest, InvalidAndForeignHandlesDoNotResolve) {
    using StringRegistry = Registry<std::string, BindingHandle>;
    auto registry_result = StringRegistry::Create(1);
    ASSERT_TRUE(registry_result.has_value());
    auto registry = std::move(registry_result.value());

    EXPECT_EQ(registry.Find(BindingHandle{}), nullptr);
    EXPECT_EQ(
        registry.Find(BindingHandle::FromRaw(std::numeric_limits<std::uint64_t>::max())), nullptr);
}

TEST(RegistryTest, SupportsNonPowerOfTwoCapacity) {
    auto registry_result = IntRegistry::Create(3);
    ASSERT_TRUE(registry_result.has_value());
    auto registry = std::move(registry_result.value());

    std::vector<StreamHandle> handles;
    for (int value = 0; value < 3; ++value) {
        auto handle = registry.Insert(value);
        ASSERT_TRUE(handle.has_value());
        handles.push_back(handle.value());
    }
    for (int value = 0; value < 3; ++value) {
        const auto* found = registry.Find(handles[static_cast<std::size_t>(value)]);
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(*found, value);
    }
}

static_assert(std::is_move_constructible<IntRegistry>::value);
static_assert(!std::is_copy_constructible<IntRegistry>::value);

TEST(RegistryTest, DirectlyUsesRealRuntimeHandleTypes) {
    auto sources = Registry<int, SourceHandle>::Create(1);
    auto streams = Registry<int, StreamHandle>::Create(1);
    auto bindings = Registry<int, BindingHandle>::Create(1);
    ASSERT_TRUE(sources.has_value());
    ASSERT_TRUE(streams.has_value());
    ASSERT_TRUE(bindings.has_value());

    const auto source = sources.value().Insert(1);
    const auto stream = streams.value().Insert(2);
    const auto binding = bindings.value().Insert(3);
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(stream.has_value());
    ASSERT_TRUE(binding.has_value());
    EXPECT_EQ(*sources.value().Find(source.value()), 1);
    EXPECT_EQ(*streams.value().Find(stream.value()), 2);
    EXPECT_EQ(*bindings.value().Find(binding.value()), 3);
}

TEST(RegistryTest, GenerationExhaustionRetiresSlotWithoutWrapping) {
    auto registry_result = IntRegistry::Create(1, std::numeric_limits<std::uint32_t>::max());
    ASSERT_TRUE(registry_result.has_value());
    auto registry = std::move(registry_result.value());

    auto handle = registry.Insert(1);
    ASSERT_TRUE(handle.has_value());
    EXPECT_EQ(handle.value().Value() >> 32U, std::numeric_limits<std::uint32_t>::max());
    ASSERT_TRUE(registry.Erase(handle.value()).has_value());
    EXPECT_EQ(registry.Size(), 0U);

    const auto retired = registry.Insert(2);
    ASSERT_FALSE(retired.has_value());
    EXPECT_EQ(retired.error().code(), ErrorCode::ResourceExhausted);
}

struct ThrowingValue {
    enum class Failure {
        None,
        RuntimeError,
        BadAlloc,
    };

    static Failure failure;

    int value = 0;

    explicit ThrowingValue(int input) : value(input) {}
    ThrowingValue(ThrowingValue&& other) : value(other.value) {
        if (failure == Failure::RuntimeError) {
            throw std::runtime_error("stored value construction failed");
        }
        if (failure == Failure::BadAlloc) {
            throw std::bad_alloc();
        }
    }
    ThrowingValue& operator=(ThrowingValue&&) = default;
};

ThrowingValue::Failure ThrowingValue::failure = ThrowingValue::Failure::None;

struct BlockingValue {
    static std::atomic<bool> armed;
    static std::atomic<bool> entered;
    static std::atomic<bool> released;

    explicit BlockingValue(int input) : value(input) {}

    BlockingValue(const BlockingValue&) = delete;
    BlockingValue& operator=(const BlockingValue&) = delete;
    BlockingValue(BlockingValue&& other) : value(other.value) {
        if (armed.exchange(false, std::memory_order_acq_rel)) {
            entered.store(true, std::memory_order_release);
            while (!released.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    }
    BlockingValue& operator=(BlockingValue&&) = delete;

    int value = 0;
};

std::atomic<bool> BlockingValue::armed{false};
std::atomic<bool> BlockingValue::entered{false};
std::atomic<bool> BlockingValue::released{false};

TEST(RegistryTest, FailedValueConstructionDoesNotLoseCapacity) {
    using ThrowingRegistry = Registry<ThrowingValue, StreamHandle>;
    auto registry_result = ThrowingRegistry::Create(1);
    ASSERT_TRUE(registry_result.has_value());
    auto registry = std::move(registry_result.value());

    ThrowingValue::failure = ThrowingValue::Failure::RuntimeError;
    const auto failed = registry.Insert(ThrowingValue(1));
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::InternalError);
    EXPECT_EQ(registry.Size(), 0U);

    ThrowingValue::failure = ThrowingValue::Failure::BadAlloc;
    const auto allocation_failed = registry.Insert(ThrowingValue(2));
    ASSERT_FALSE(allocation_failed.has_value());
    EXPECT_EQ(allocation_failed.error().code(), ErrorCode::ResourceExhausted);
    EXPECT_EQ(registry.Size(), 0U);

    ThrowingValue::failure = ThrowingValue::Failure::None;
    const auto valid = registry.Insert(ThrowingValue(3));
    ASSERT_TRUE(valid.has_value());
    ASSERT_NE(registry.Find(valid.value()), nullptr);
    EXPECT_EQ(registry.Find(valid.value())->value, 3);
}

TEST(RegistryTest, UnpublishedSlotCannotBeObservedDuringConcurrentInsert) {
    using BlockingRegistry = Registry<BlockingValue, StreamHandle>;
    auto registry_result = BlockingRegistry::Create(1);
    ASSERT_TRUE(registry_result.has_value());
    auto registry = std::move(registry_result.value());

    BlockingValue::armed.store(true, std::memory_order_release);
    BlockingValue::entered.store(false, std::memory_order_release);
    BlockingValue::released.store(false, std::memory_order_release);
    std::optional<Result<StreamHandle>> inserted;
    std::thread writer(
        [&registry, &inserted] { inserted.emplace(registry.Insert(BlockingValue{7})); });
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!BlockingValue::entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(BlockingValue::entered.load(std::memory_order_acquire));

    const StreamHandle unpublished = StreamHandle::FromRaw((std::uint64_t{1} << 32U) | 1U);
    for (std::size_t attempt = 0; attempt < 1'000; ++attempt) {
        ASSERT_EQ(registry.FindStable(unpublished), nullptr);
    }

    BlockingValue::released.store(true, std::memory_order_release);
    writer.join();
    ASSERT_TRUE(inserted.has_value());
    ASSERT_TRUE(inserted->has_value());
    EXPECT_EQ(inserted->value(), unpublished);
    const auto* published = registry.FindStable(unpublished);
    ASSERT_NE(published, nullptr);
    EXPECT_EQ(published->value, 7);
}

TEST(RegistryTest, PublishedSlotsRemainVisibleDuringLaterConcurrentInsert) {
    constexpr std::size_t kCount = 64;
    auto registry_result = IntRegistry::Create(kCount);
    ASSERT_TRUE(registry_result.has_value());
    auto registry = std::move(registry_result.value());

    std::array<StreamHandle, kCount> handles{};
    std::atomic<std::size_t> published{0};
    std::thread writer([&registry, &handles, &published] {
        for (std::size_t index = 0; index < kCount; ++index) {
            auto handle = registry.Insert(static_cast<int>(index));
            if (!handle.has_value()) {
                published.store(kCount + 1U, std::memory_order_release);
                return;
            }
            handles[index] = handle.value();
            published.store(index + 1U, std::memory_order_release);
        }
    });

    std::size_t observed = 0;
    while (true) {
        observed = published.load(std::memory_order_acquire);
        const std::size_t visible = std::min(observed, kCount);
        for (std::size_t index = 0; index < visible; ++index) {
            const auto* value = registry.FindStable(handles[index]);
            ASSERT_NE(value, nullptr);
            EXPECT_EQ(*value, static_cast<int>(index));
        }
        if (observed >= kCount) {
            break;
        }
        std::this_thread::yield();
    }
    writer.join();
    ASSERT_EQ(observed, kCount);
}

}  // namespace
}  // namespace rodaq
