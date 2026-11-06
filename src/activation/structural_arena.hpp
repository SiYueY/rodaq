#ifndef RODAQ_ACTIVATION_STRUCTURAL_ARENA_HPP_
#define RODAQ_ACTIVATION_STRUCTURAL_ARENA_HPP_

#include <rodaq/result.hpp>

#include <cstddef>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>

namespace rodaq {

/// A bounded, monotonic arena for activation-lifetime structural metadata.
///
/// The arena never falls back to an upstream heap. Allocation is externally
/// serialized; returned addresses remain stable until the arena is destroyed.
class StructuralArena final {
public:
    static Result<std::unique_ptr<StructuralArena>> create(std::size_t capacity_bytes);

    StructuralArena(const StructuralArena&) = delete;
    StructuralArena& operator=(const StructuralArena&) = delete;
    StructuralArena(StructuralArena&&) = delete;
    StructuralArena& operator=(StructuralArena&&) = delete;
    ~StructuralArena();

    Result<void*> allocate_bytes(
        std::size_t size, std::size_t alignment = alignof(std::max_align_t));
    std::pmr::memory_resource* resource() noexcept;

    template <typename T>
    Result<T*> allocate(std::size_t count = 1) {
        if (count == 0) {
            return static_cast<T*>(nullptr);
        }
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            return rodaq::Status(StatusCode::InvalidArgument, "arena allocation size overflows");
        }
        const auto bytes = allocate_bytes(count * sizeof(T), alignof(T));
        if (!bytes.ok()) {
            return bytes.status();
        }
        return static_cast<T*>(bytes.value());
    }

    std::size_t capacity_bytes() const noexcept;

private:
    class Impl;

    explicit StructuralArena(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq

#endif  // RODAQ_ACTIVATION_STRUCTURAL_ARENA_HPP_
