#ifndef RODAQ_RUNTIME_HANDLE_HPP_
#define RODAQ_RUNTIME_HANDLE_HPP_

#include <rodaq/result.hpp>

#include <cstdint>
#include <limits>
#include <type_traits>

namespace rodaq {

/// A lightweight, activation-local identity token.
///
/// Handle values are not Canonical identifiers and must not be persisted.
template <typename Tag>
class Handle final {
public:
    constexpr Handle() noexcept = default;

    static constexpr Handle from_raw(std::uint64_t value) noexcept { return Handle(value); }

    constexpr bool valid() const noexcept { return value_ != 0; }

    constexpr std::uint64_t value() const noexcept { return value_; }

    friend constexpr bool operator==(Handle lhs, Handle rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }

    friend constexpr bool operator!=(Handle lhs, Handle rhs) noexcept { return !(lhs == rhs); }

private:
    explicit constexpr Handle(std::uint64_t value) noexcept : value_(value) {}

    std::uint64_t value_ = 0;
};

/// Allocates monotonic, non-zero handles without reuse.
///
/// Allocation is externally serialized for a given allocator instance.
template <typename HandleType>
class HandleAllocator final {
public:
    explicit constexpr HandleAllocator(std::uint64_t first_value = 1) noexcept
    : next_value_(first_value) {}

    Result<HandleType> allocate() {
        if (next_value_ == 0) {
            return rodaq::Status(
                StatusCode::ResourceExhausted, "runtime handle identity space is exhausted");
        }

        const std::uint64_t value = next_value_;
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            next_value_ = 0;
        } else {
            ++next_value_;
        }
        return HandleType::from_raw(value);
    }

private:
    std::uint64_t next_value_ = 1;
};

struct SourceHandleTag;
struct ClockHandleTag;
struct SchemaHandleTag;
struct TimestampKeySetHandleTag;
struct StreamHandleTag;
struct StreamDescriptorHandleTag;
struct BindingHandleTag;

using SourceHandle = Handle<SourceHandleTag>;
using ClockHandle = Handle<ClockHandleTag>;
using SchemaHandle = Handle<SchemaHandleTag>;
using TimestampKeySetHandle = Handle<TimestampKeySetHandleTag>;
using StreamHandle = Handle<StreamHandleTag>;
using StreamDescriptorHandle = Handle<StreamDescriptorHandleTag>;
using BindingHandle = Handle<BindingHandleTag>;

static_assert(std::is_trivially_copyable<StreamHandle>::value);
static_assert(std::is_standard_layout<StreamHandle>::value);
static_assert(!std::is_convertible<std::uint64_t, StreamHandle>::value);

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_HANDLE_HPP_
