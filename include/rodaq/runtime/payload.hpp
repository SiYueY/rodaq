#ifndef RODAQ_RUNTIME_PAYLOAD_HPP_
#define RODAQ_RUNTIME_PAYLOAD_HPP_

#include <rodaq/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rodaq {

class PayloadPoolState;

using PayloadRelease = void (*)(void* owner_context) noexcept;

/// A non-owning view of one contiguous immutable byte range.
class PayloadView final {
public:
    constexpr PayloadView() noexcept = default;

    constexpr PayloadView(const std::uint8_t* data, std::size_t size) noexcept
    : data_(size == 0 ? nullptr : data), size_(data == nullptr ? 0 : size) {}

    constexpr const std::uint8_t* Data() const noexcept { return data_; }

    constexpr std::size_t Size() const noexcept { return size_; }

    constexpr bool Empty() const noexcept { return size_ == 0; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

/// Owns one immutable contiguous Payload backing allocation.
///
/// The physical backing storage is returned to its originating pool exactly
/// once. Move operations transfer ownership and leave the source empty.
class Payload final {
public:
    static Result<Payload> Adopt(PayloadView view, void* owner_context, PayloadRelease release);
    static Result<Payload> Retain(PayloadView view, void* owner_context, PayloadRelease release);

    Payload() noexcept = default;
    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;
    Payload(Payload&& other) noexcept;
    Payload& operator=(Payload&& other) noexcept;
    ~Payload();

    PayloadView View() const noexcept;
    std::size_t Size() const noexcept;
    bool Empty() const noexcept;

private:
    friend class PayloadPool;
    friend class PayloadPoolState;

    Payload(
        std::shared_ptr<PayloadPoolState> pool_state, std::uint8_t* data, std::size_t size,
        std::size_t block_index) noexcept;
    Payload(
        const std::uint8_t* data, std::size_t size, void* owner_context,
        PayloadRelease release) noexcept;

    void ReleaseCurrent() noexcept;

    std::shared_ptr<PayloadPoolState> pool_state_;
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t block_index_ = 0;
    void* owner_context_ = nullptr;
    PayloadRelease release_ = nullptr;
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_PAYLOAD_HPP_
