#ifndef RODAQ_RUNTIME_REGISTRY_HPP_
#define RODAQ_RUNTIME_REGISTRY_HPP_

#include <rodaq/runtime/handle.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rodaq {

/// A fixed-capacity registry with generation-protected handles.
///
/// Mutation must be externally serialized. Concurrent lookups are safe only
/// while no insertion, erasure, movement, or destruction is in progress.
template <typename T>
class Registry final {
public:
    struct Tag;
    using HandleType = Handle<Tag>;

    static Result<Registry> Create(std::size_t capacity) {
        constexpr std::size_t kMaxCapacity =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - 1U);
        if (capacity > kMaxCapacity) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidArgument, "registry capacity exceeds the handle slot domain"));
        }
        try {
            return Registry(capacity);
        } catch (const std::bad_alloc&) {
            return nonstd::make_unexpected(
                Error(ErrorCode::ResourceExhausted, "failed to allocate registry storage"));
        } catch (const std::length_error&) {
            return nonstd::make_unexpected(
                Error(ErrorCode::ResourceExhausted, "registry capacity exceeds container limits"));
        }
    }

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) noexcept = default;
    Registry& operator=(Registry&&) noexcept = default;

    Result<HandleType> Insert(T value) {
        if (free_slots_.empty()) {
            return nonstd::make_unexpected(
                Error(ErrorCode::ResourceExhausted, "registry capacity is exhausted"));
        }

        const std::size_t index = free_slots_.back();
        free_slots_.pop_back();
        Slot& slot = slots_[index];
        slot.value.emplace(std::move(value));
        ++size_;
        return HandleType::FromRaw(encode(index, slot.generation));
    }

    T* Find(HandleType handle) noexcept {
        return const_cast<T*>(static_cast<const Registry&>(*this).Find(handle));
    }

    const T* Find(HandleType handle) const noexcept {
        if (!handle.Valid()) {
            return nullptr;
        }

        const std::uint64_t raw_index = (handle.Value() & kIndexMask) - 1U;
        if (raw_index >= slots_.size()) {
            return nullptr;
        }

        const std::size_t index = static_cast<std::size_t>(raw_index);
        const Slot& slot = slots_[index];
        if (!slot.value.has_value() ||
            slot.generation != static_cast<std::uint32_t>(handle.Value() >> kIndexBits)) {
            return nullptr;
        }
        return &slot.value.value();
    }

    Result<void> Erase(HandleType handle) {
        if (!handle.Valid()) {
            return nonstd::make_unexpected(
                Error(ErrorCode::InvalidArgument, "registry handle is invalid"));
        }

        const std::uint64_t raw_index = (handle.Value() & kIndexMask) - 1U;
        if (raw_index >= slots_.size()) {
            return nonstd::make_unexpected(
                Error(ErrorCode::NotFound, "registry handle was not found"));
        }

        const std::size_t index = static_cast<std::size_t>(raw_index);
        Slot& slot = slots_[index];
        if (!slot.value.has_value() ||
            slot.generation != static_cast<std::uint32_t>(handle.Value() >> kIndexBits)) {
            return nonstd::make_unexpected(
                Error(ErrorCode::NotFound, "registry handle was not found"));
        }

        slot.value.reset();
        --size_;
        if (slot.generation != std::numeric_limits<std::uint32_t>::max()) {
            ++slot.generation;
            free_slots_.push_back(index);
        }
        return {};
    }

    std::size_t Size() const noexcept { return size_; }

    std::size_t Capacity() const noexcept { return slots_.size(); }

private:
    static constexpr std::uint64_t kIndexBits = 32;
    static constexpr std::uint64_t kIndexMask = (std::uint64_t{1} << kIndexBits) - 1U;

    struct Slot {
        std::optional<T> value;
        std::uint32_t generation = 1;
    };

    explicit Registry(std::size_t capacity) : slots_(capacity), free_slots_(), size_(0) {
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
