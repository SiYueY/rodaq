#ifndef RODAQ_RUNTIME_REGISTRY_HPP_
#define RODAQ_RUNTIME_REGISTRY_HPP_

#include <rodaq/runtime/handle.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rodaq {

/// A fixed-capacity registry with generation-protected handles.
///
/// Mutation must be externally serialized. Concurrent lookups are safe only
/// while no insertion, erasure, movement, or destruction is in progress.
///
/// Registry generation prevents stale handles from aliasing within this
/// Registry's lifetime. Cross-activation validation remains the responsibility
/// of the M2 ActiveRuntimeState that owns the Registry.
template <typename T, typename HandleType>
class Registry final {
public:
    static Result<Registry> create(std::size_t capacity, std::uint32_t initial_generation = 1) {
        constexpr std::size_t kMaxCapacity =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - 1U);
        if (capacity > kMaxCapacity) {
            return rodaq::Status(
                StatusCode::InvalidArgument, "registry capacity exceeds the handle slot domain");
        }
        try {
            if (initial_generation == 0) {
                return rodaq::Status(
                    StatusCode::InvalidArgument, "registry initial generation must be non-zero");
            }
            return Registry(capacity, initial_generation);
        } catch (const std::bad_alloc&) {
            return rodaq::Status(
                StatusCode::ResourceExhausted, "failed to allocate registry storage");
        } catch (const std::length_error&) {
            return rodaq::Status(
                StatusCode::ResourceExhausted, "registry capacity exceeds container limits");
        }
    }

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) noexcept = default;
    Registry& operator=(Registry&&) noexcept = default;

    Result<HandleType> insert(T value) {
        if (free_slots_.empty()) {
            return rodaq::Status(StatusCode::ResourceExhausted, "registry capacity is exhausted");
        }

        const std::size_t index = free_slots_.back();
        Slot& slot = slots_[index];
        try {
            slot.value.emplace(std::move(value));
        } catch (const std::bad_alloc&) {
            return rodaq::Status(
                StatusCode::ResourceExhausted, "registry value construction exhausted memory");
        } catch (const std::length_error&) {
            return rodaq::Status(
                StatusCode::ResourceExhausted,
                "registry value construction exceeded a length limit");
        } catch (const std::exception&) {
            return rodaq::Status(StatusCode::InternalError, "registry value construction failed");
        } catch (...) {
            return rodaq::Status(StatusCode::InternalError, "registry value construction failed");
        }

        slot.published.store(true, std::memory_order_release);
        free_slots_.pop_back();
        ++size_;
        return HandleType::from_raw(encode(index, slot.generation));
    }

    T* find(HandleType handle) noexcept {
        return const_cast<T*>(static_cast<const Registry&>(*this).find(handle));
    }

    const T* find(HandleType handle) const noexcept {
        if (!handle.valid()) {
            return nullptr;
        }

        const std::uint64_t raw_index = (handle.value() & kIndexMask) - 1U;
        if (raw_index >= slots_.size()) {
            return nullptr;
        }

        const std::size_t index = static_cast<std::size_t>(raw_index);
        const Slot& slot = slots_[index];
        if (!slot.published.load(std::memory_order_acquire) ||
            slot.generation != static_cast<std::uint32_t>(handle.value() >> kIndexBits)) {
            return nullptr;
        }
        return &slot.value.value();
    }

    /// Looks up a handle in an insert-only Registry whose slots are never erased
    /// or reused during the caller's lifetime.
    ///
    /// Insert release-publishes a fully constructed slot and this lookup
    /// acquire-observes that publication. Callers must not invoke this
    /// concurrently with Erase. Insertion into a different preallocated slot is
    /// safe. Values referenced through this lookup must be immutable or use
    /// their own synchronization for mutable fields.
    const T* find_stable(HandleType handle) const noexcept {
        if (!handle.valid()) {
            return nullptr;
        }
        const std::uint64_t raw_index = (handle.value() & kIndexMask) - 1U;
        if (raw_index >= slots_.size()) {
            return nullptr;
        }
        const std::size_t index = static_cast<std::size_t>(raw_index);
        const Slot& slot = slots_[index];
        if (!slot.published.load(std::memory_order_acquire) ||
            slot.generation != static_cast<std::uint32_t>(handle.value() >> kIndexBits)) {
            return nullptr;
        }
        return &slot.value.value();
    }

    T* find_stable(HandleType handle) noexcept {
        return const_cast<T*>(static_cast<const Registry&>(*this).find_stable(handle));
    }

    Result<void> erase(HandleType handle) {
        if (!handle.valid()) {
            return Status(StatusCode::InvalidArgument, "registry handle is invalid");
        }

        const std::uint64_t raw_index = (handle.value() & kIndexMask) - 1U;
        if (raw_index >= slots_.size()) {
            return Status(StatusCode::NotFound, "registry handle was not found");
        }

        const std::size_t index = static_cast<std::size_t>(raw_index);
        Slot& slot = slots_[index];
        if (!slot.published.load(std::memory_order_acquire) ||
            slot.generation != static_cast<std::uint32_t>(handle.value() >> kIndexBits)) {
            return Status(StatusCode::NotFound, "registry handle was not found");
        }

        slot.published.store(false, std::memory_order_release);
        slot.value.reset();
        --size_;
        if (slot.generation != std::numeric_limits<std::uint32_t>::max()) {
            ++slot.generation;
            free_slots_.push_back(index);
        }
        return {};
    }

    std::size_t size() const noexcept { return size_; }

    std::size_t capacity() const noexcept { return slots_.size(); }

private:
    static constexpr std::uint64_t kIndexBits = 32;
    static constexpr std::uint64_t kIndexMask = (std::uint64_t{1} << kIndexBits) - 1U;

    struct Slot {
        std::optional<T> value;
        std::atomic<bool> published{false};
        std::uint32_t generation = 1;
    };

    explicit Registry(std::size_t capacity, std::uint32_t initial_generation)
    : slots_(capacity), free_slots_(), size_(0) {
        for (Slot& slot : slots_) {
            slot.generation = initial_generation;
        }
        free_slots_.reserve(capacity);
        for (std::size_t offset = 0; offset < capacity; ++offset) {
            free_slots_.push_back(capacity - offset - 1U);
        }
    }

    static std::uint64_t encode(std::size_t index, std::uint32_t generation) noexcept {
        return (static_cast<std::uint64_t>(generation) << kIndexBits) |
               (static_cast<std::uint64_t>(index) + 1U);
    }

    std::vector<Slot> slots_;
    std::vector<std::size_t> free_slots_;
    std::size_t size_ = 0;
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_REGISTRY_HPP_
