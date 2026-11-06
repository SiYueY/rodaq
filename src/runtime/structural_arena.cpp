#include "rodaq/runtime/structural_arena.hpp"

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <new>

namespace rodaq {

class StructuralArena::Impl final {
public:
    explicit Impl(std::size_t capacity_bytes)
    : buffer_(std::make_unique<std::byte[]>(capacity_bytes)),
      resource_(buffer_.get(), capacity_bytes, std::pmr::null_memory_resource()),
      capacity_bytes_(capacity_bytes) {}

    Result<void*> allocate_bytes(std::size_t size, std::size_t alignment) {
        if (size == 0) {
            return static_cast<void*>(nullptr);
        }
        if (alignment == 0 || (alignment & (alignment - 1U)) != 0) {
            return rodaq::Status(
                StatusCode::InvalidArgument, "arena alignment must be a power of two");
        }

        try {
            return resource_.allocate(size, alignment);
        } catch (const std::bad_alloc&) {
            return rodaq::Status(
                StatusCode::ResourceExhausted, "structural arena capacity is exhausted");
        }
    }

    std::size_t capacity_bytes() const noexcept { return capacity_bytes_; }

    std::pmr::memory_resource* resource() noexcept { return &resource_; }

private:
    std::unique_ptr<std::byte[]> buffer_;
    std::pmr::monotonic_buffer_resource resource_;
    std::size_t capacity_bytes_ = 0;
};

Result<std::unique_ptr<StructuralArena>> StructuralArena::create(std::size_t capacity_bytes) {
    try {
        auto impl = std::make_unique<Impl>(capacity_bytes);
        return std::unique_ptr<StructuralArena>(new StructuralArena(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return rodaq::Status(
            StatusCode::ResourceExhausted, "failed to allocate structural arena storage");
    }
}

StructuralArena::StructuralArena(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

StructuralArena::~StructuralArena() = default;

Result<void*> StructuralArena::allocate_bytes(std::size_t size, std::size_t alignment) {
    return impl_->allocate_bytes(size, alignment);
}

std::size_t StructuralArena::capacity_bytes() const noexcept { return impl_->capacity_bytes(); }

std::pmr::memory_resource* StructuralArena::resource() noexcept { return impl_->resource(); }

}  // namespace rodaq
