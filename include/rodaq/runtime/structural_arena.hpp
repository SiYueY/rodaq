#ifndef RODAQ_RUNTIME_STRUCTURAL_ARENA_HPP_
#define RODAQ_RUNTIME_STRUCTURAL_ARENA_HPP_

#include <rodaq/result.hpp>

#include <cstddef>
#include <limits>
#include <memory>
#include <new>

namespace rodaq {

/// A bounded, monotonic arena for activation-lifetime structural metadata.
///
/// The arena never falls back to an upstream heap. Allocation is externally
/// serialized; returned addresses remain stable until the arena is destroyed.
class StructuralArena final {
public:
    static Result<std::unique_ptr<StructuralArena>> Create(std::size_t capacity_bytes);

    StructuralArena(const StructuralArena&) = delete;
    StructuralArena& operator=(const StructuralArena&) = delete;
    StructuralArena(StructuralArena&&) = delete;
    StructuralArena& operator=(StructuralArena&&) = delete;
    ~StructuralArena();

    Result<void*> AllocateBytes(
        std::size_t size, std::size_t alignment = alignof(std::max_align_t));

    template <typename T>
    Result<T*> Allocate(std::size_t count = 1) {
        if (count == 0) {
            return static_cast<T*>(nullptr);
        }
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            return nonstd::make_unexpected(
                Error(ErrorCode::InvalidArgument, "arena allocation size overflows"));
        }
        const auto bytes = AllocateBytes(count * sizeof(T), alignof(T));
        if (!bytes.has_value()) {
            return nonstd::make_unexpected(bytes.error());
        }
        return static_cast<T*>(bytes.value());
    }

    std::size_t CapacityBytes() const noexcept;

private:
    class Impl;

    explicit StructuralArena(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_STRUCTURAL_ARENA_HPP_
