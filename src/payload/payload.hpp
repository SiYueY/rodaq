#ifndef RODAQ_PAYLOAD_HPP_
#define RODAQ_PAYLOAD_HPP_

#include <rodaq/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rodaq {

class PayloadPoolState;
class ActivationState;

using PayloadRelease = void (*)(void* owner_context) noexcept;

/// A non-owning view of one contiguous immutable byte range.
class PayloadView final {
public:
    constexpr PayloadView() noexcept = default;

    constexpr PayloadView(const std::uint8_t* data, std::size_t size) noexcept
    : data_(data), size_(size) {}

    constexpr const std::uint8_t* data() const noexcept { return data_; }

    constexpr std::size_t size() const noexcept { return size_; }

    constexpr bool empty() const noexcept { return size_ == 0; }

    constexpr bool valid() const noexcept { return size_ == 0 || data_ != nullptr; }

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
    static Result<Payload> adopt(PayloadView view, void* owner_context, PayloadRelease release);
    static Result<Payload> retain(PayloadView view, void* owner_context, PayloadRelease release);

    Payload() noexcept = default;
    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;
    Payload(Payload&& other) noexcept;
    Payload& operator=(Payload&& other) noexcept;
    ~Payload();

    PayloadView view() const noexcept;
    std::size_t size() const noexcept;
    bool empty() const noexcept;
    bool requires_external_release() const noexcept;
    /// Attaches non-owning-domain lifetime state that must outlive the release
    /// callback. DAQ uses this for the Acquisition Plugin instance lease.
    void attach_lifetime_guard(std::shared_ptr<void> lifetime_guard) noexcept;

private:
    friend class PayloadPool;
    friend class PayloadPoolState;
    friend class ActivationState;

    Payload(
        std::shared_ptr<PayloadPoolState> pool_state, std::uint8_t* data, std::size_t size,
        std::size_t class_index, std::size_t block_index) noexcept;
    Payload(
        const std::uint8_t* data, std::size_t size, void* owner_context,
        PayloadRelease release) noexcept;

    void release_current() noexcept;
    std::shared_ptr<PayloadPoolState> pool_state_;
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t block_class_ = 0;
    std::size_t block_index_ = 0;
    void* owner_context_ = nullptr;
    PayloadRelease release_ = nullptr;
    std::shared_ptr<void> lifetime_guard_;
};

}  // namespace rodaq

#endif  // RODAQ_PAYLOAD_HPP_
