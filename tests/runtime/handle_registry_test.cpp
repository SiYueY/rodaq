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
    EXPECT_FALSE(handle.valid());
    EXPECT_EQ(handle.value(), 0U);
}

TEST(HandleTest, RawHandleAndEquality) {
    const auto first = StreamHandle::from_raw(42);
    const auto second = StreamHandle::from_raw(42);
    const auto other = StreamHandle::from_raw(43);

    EXPECT_TRUE(first.valid());
    EXPECT_EQ(first, second);
    EXPECT_NE(first, other);
    EXPECT_EQ(first.value(), 42U);
}

TEST(HandleTest, AllocatorIsMonotonicAndDetectsOverflow) {
    HandleAllocator<StreamHandle> allocator(std::numeric_limits<std::uint64_t>::max());

    const auto last = allocator.allocate();
    ASSERT_TRUE(last.ok());
    EXPECT_EQ(last.value().value(), std::numeric_limits<std::uint64_t>::max());

    const auto exhausted = allocator.allocate();
    ASSERT_FALSE(exhausted.ok());
    EXPECT_EQ(exhausted.status().status_code(), StatusCode::ResourceExhausted);
}

TEST(RegistryTest, InsertFindEraseAndCapacity) {
    auto registry_result = IntRegistry::create(2);
    ASSERT_TRUE(registry_result.ok());
    auto registry = std::move(registry_result.value());

    auto first = registry.insert(10);
    auto second = registry.insert(20);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_NE(first.value(), second.value());
    EXPECT_EQ(registry.size(), 2U);
    EXPECT_EQ(registry.capacity(), 2U);
    ASSERT_NE(registry.find(first.value()), nullptr);
    EXPECT_EQ(*registry.find(first.value()), 10);

    const auto full = registry.insert(30);
    ASSERT_FALSE(full.ok());
    EXPECT_EQ(full.status().status_code(), StatusCode::ResourceExhausted);

    ASSERT_TRUE(registry.erase(first.value()).ok());
    EXPECT_EQ(registry.find(first.value()), nullptr);
    EXPECT_EQ(registry.size(), 1U);
}

TEST(RegistryTest, EraseRejectsInvalidAndMissingHandles) {
    auto registry_result = IntRegistry::create(1);
    ASSERT_TRUE(registry_result.ok());
    auto registry = std::move(registry_result.value());

    const auto invalid = registry.erase(StreamHandle{});
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.status().status_code(), StatusCode::InvalidArgument);

    auto handle = registry.insert(1);
    ASSERT_TRUE(handle.ok());
    ASSERT_TRUE(registry.erase(handle.value()).ok());

    const auto double_erase = registry.erase(handle.value());
    ASSERT_FALSE(double_erase.ok());
    EXPECT_EQ(double_erase.status().status_code(), StatusCode::NotFound);
}

TEST(RegistryTest, StaleHandleCannotResolveReusedSlot) {
    auto registry_result = IntRegistry::create(1);
    ASSERT_TRUE(registry_result.ok());
    auto registry = std::move(registry_result.value());

    auto old_handle = registry.insert(1);
    ASSERT_TRUE(old_handle.ok());
    ASSERT_TRUE(registry.erase(old_handle.value()).ok());

    auto new_handle = registry.insert(2);
    ASSERT_TRUE(new_handle.ok());
    EXPECT_NE(old_handle.value(), new_handle.value());
    EXPECT_EQ(registry.find(old_handle.value()), nullptr);
    ASSERT_NE(registry.find(new_handle.value()), nullptr);
    EXPECT_EQ(*registry.find(new_handle.value()), 2);
}

TEST(RegistryTest, InvalidAndForeignHandlesDoNotResolve) {
    using StringRegistry = Registry<std::string, BindingHandle>;
    auto registry_result = StringRegistry::create(1);
    ASSERT_TRUE(registry_result.ok());
    auto registry = std::move(registry_result.value());

    EXPECT_EQ(registry.find(BindingHandle{}), nullptr);
    EXPECT_EQ(
        registry.find(BindingHandle::from_raw(std::numeric_limits<std::uint64_t>::max())), nullptr);
}

TEST(RegistryTest, SupportsNonPowerOfTwoCapacity) {
    auto registry_result = IntRegistry::create(3);
    ASSERT_TRUE(registry_result.ok());
    auto registry = std::move(registry_result.value());

    std::vector<StreamHandle> handles;
    for (int value = 0; value < 3; ++value) {
        auto handle = registry.insert(value);
        ASSERT_TRUE(handle.ok());
        handles.push_back(handle.value());
    }
    for (int value = 0; value < 3; ++value) {
        const auto* found = registry.find(handles[static_cast<std::size_t>(value)]);
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(*found, value);
    }
}

static_assert(std::is_move_constructible<IntRegistry>::value);
static_assert(!std::is_copy_constructible<IntRegistry>::value);

TEST(RegistryTest, DirectlyUsesRealRuntimeHandleTypes) {
    auto sources = Registry<int, SourceHandle>::create(1);
    auto streams = Registry<int, StreamHandle>::create(1);
    auto bindings = Registry<int, BindingHandle>::create(1);
    ASSERT_TRUE(sources.ok());
    ASSERT_TRUE(streams.ok());
    ASSERT_TRUE(bindings.ok());

    const auto source = sources.value().insert(1);
    const auto stream = streams.value().insert(2);
    const auto binding = bindings.value().insert(3);
    ASSERT_TRUE(source.ok());
    ASSERT_TRUE(stream.ok());
    ASSERT_TRUE(binding.ok());
    EXPECT_EQ(*sources.value().find(source.value()), 1);
    EXPECT_EQ(*streams.value().find(stream.value()), 2);
    EXPECT_EQ(*bindings.value().find(binding.value()), 3);
}

TEST(RegistryTest, GenerationExhaustionRetiresSlotWithoutWrapping) {
    auto registry_result = IntRegistry::create(1, std::numeric_limits<std::uint32_t>::max());
    ASSERT_TRUE(registry_result.ok());
    auto registry = std::move(registry_result.value());

    auto handle = registry.insert(1);
    ASSERT_TRUE(handle.ok());
    EXPECT_EQ(handle.value().value() >> 32U, std::numeric_limits<std::uint32_t>::max());
    ASSERT_TRUE(registry.erase(handle.value()).ok());
    EXPECT_EQ(registry.size(), 0U);

    const auto retired = registry.insert(2);
    ASSERT_FALSE(retired.ok());
    EXPECT_EQ(retired.status().status_code(), StatusCode::ResourceExhausted);
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
    auto registry_result = ThrowingRegistry::create(1);
    ASSERT_TRUE(registry_result.ok());
    auto registry = std::move(registry_result.value());

    ThrowingValue::failure = ThrowingValue::Failure::RuntimeError;
    const auto failed = registry.insert(ThrowingValue(1));
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.status().status_code(), StatusCode::InternalError);
    EXPECT_EQ(registry.size(), 0U);

    ThrowingValue::failure = ThrowingValue::Failure::BadAlloc;
    const auto allocation_failed = registry.insert(ThrowingValue(2));
    ASSERT_FALSE(allocation_failed.ok());
    EXPECT_EQ(allocation_failed.status().status_code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(registry.size(), 0U);

    ThrowingValue::failure = ThrowingValue::Failure::None;
    const auto valid = registry.insert(ThrowingValue(3));
    ASSERT_TRUE(valid.ok());
    ASSERT_NE(registry.find(valid.value()), nullptr);
    EXPECT_EQ(registry.find(valid.value())->value, 3);
}

TEST(RegistryTest, UnpublishedSlotCannotBeObservedDuringConcurrentInsert) {
    using BlockingRegistry = Registry<BlockingValue, StreamHandle>;
    auto registry_result = BlockingRegistry::create(1);
    ASSERT_TRUE(registry_result.ok());
    auto registry = std::move(registry_result.value());

    BlockingValue::armed.store(true, std::memory_order_release);
    BlockingValue::entered.store(false, std::memory_order_release);
    BlockingValue::released.store(false, std::memory_order_release);
    std::optional<Result<StreamHandle>> inserted;
    std::thread writer(
        [&registry, &inserted] { inserted.emplace(registry.insert(BlockingValue{7})); });
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!BlockingValue::entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(BlockingValue::entered.load(std::memory_order_acquire));

    const StreamHandle unpublished = StreamHandle::from_raw((std::uint64_t{1} << 32U) | 1U);
    for (std::size_t attempt = 0; attempt < 1'000; ++attempt) {
        ASSERT_EQ(registry.find_stable(unpublished), nullptr);
    }

    BlockingValue::released.store(true, std::memory_order_release);
    writer.join();
    ASSERT_TRUE(inserted.has_value());
    ASSERT_TRUE(inserted->ok());
    EXPECT_EQ(inserted->value(), unpublished);
    const auto* published = registry.find_stable(unpublished);
    ASSERT_NE(published, nullptr);
    EXPECT_EQ(published->value, 7);
}

TEST(RegistryTest, PublishedSlotsRemainVisibleDuringLaterConcurrentInsert) {
    constexpr std::size_t kCount = 64;
    auto registry_result = IntRegistry::create(kCount);
    ASSERT_TRUE(registry_result.ok());
    auto registry = std::move(registry_result.value());

    std::array<StreamHandle, kCount> handles{};
    std::atomic<std::size_t> published{0};
    std::thread writer([&registry, &handles, &published] {
        for (std::size_t index = 0; index < kCount; ++index) {
            auto handle = registry.insert(static_cast<int>(index));
            if (!handle.ok()) {
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
            const auto* value = registry.find_stable(handles[index]);
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
