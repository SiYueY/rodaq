#pragma once

#include <rodaq/plugin/c/interface.h>
#include <rodaq/result.hpp>
#include <rodaq/status.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace rodaq::plugin {

class PluginInstance;
class PluginSetup;

namespace interface_support {

inline constexpr std::size_t kErrorMessageCapacity = 512u;

inline thread_local const void* active_operation_owner = nullptr;

[[nodiscard]] inline bool in_active_operation(const void* owner) noexcept {
    return active_operation_owner != nullptr && active_operation_owner == owner;
}

[[nodiscard]] inline bool valid_reference(rodaq_interface_reference_t reference) noexcept {
    return reference.context != nullptr && reference.vtable != nullptr &&
           reference.vtable_size >= sizeof(rodaq_struct_header_t);
}

[[nodiscard]] inline bool is_known_status(rodaq_status_code_t code) noexcept {
    return code <= RODAQ_STATUS_INTERNAL;
}

[[nodiscard]] inline StatusCode status_code(rodaq_status_code_t code) noexcept {
    switch (code) {
        case RODAQ_STATUS_OK:
            return StatusCode::Ok;
        case RODAQ_STATUS_INVALID_ARGUMENT:
            return StatusCode::InvalidArgument;
        case RODAQ_STATUS_INVALID_STATE:
            return StatusCode::InvalidState;
        case RODAQ_STATUS_NOT_FOUND:
            return StatusCode::NotFound;
        case RODAQ_STATUS_CONFLICT:
            return StatusCode::Conflict;
        case RODAQ_STATUS_RESOURCE_EXHAUSTED:
            return StatusCode::ResourceExhausted;
        case RODAQ_STATUS_DEADLINE_EXCEEDED:
            return StatusCode::DeadlineExceeded;
        case RODAQ_STATUS_UNAVAILABLE:
            return StatusCode::Unavailable;
        case RODAQ_STATUS_PERMISSION_DENIED:
            return StatusCode::PermissionDenied;
        case RODAQ_STATUS_DATA_LOSS:
            return StatusCode::DataLoss;
        case RODAQ_STATUS_INTERNAL:
        default:
            return StatusCode::Internal;
    }
}

[[nodiscard]] inline Result<void> result_from_status(rodaq_status_code_t code) {
    if (!is_known_status(code)) {
        return Status{StatusCode::Internal, "plugin returned an unknown status"};
    }
    const auto converted = status_code(code);
    if (converted == StatusCode::Ok) {
        return {};
    }
    return Status{converted, "plugin interface call failed"};
}

[[nodiscard]] inline bool valid_utf8(std::string_view text) noexcept {
    std::size_t offset = 0u;
    while (offset < text.size()) {
        const auto first = static_cast<unsigned char>(text[offset]);
        std::size_t continuation_count = 0u;
        std::uint32_t scalar = 0u;
        std::uint32_t minimum = 0u;
        if (first <= 0x7fu) {
            ++offset;
            continue;
        }
        if (first >= 0xc2u && first <= 0xdfu) {
            continuation_count = 1u;
            scalar = first & 0x1fu;
            minimum = 0x80u;
        } else if (first >= 0xe0u && first <= 0xefu) {
            continuation_count = 2u;
            scalar = first & 0x0fu;
            minimum = 0x800u;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            continuation_count = 3u;
            scalar = first & 0x07u;
            minimum = 0x10000u;
        } else {
            return false;
        }
        if (offset + continuation_count >= text.size()) {
            return false;
        }
        for (std::size_t index = 1u; index <= continuation_count; ++index) {
            const auto continuation = static_cast<unsigned char>(text[offset + index]);
            if ((continuation & 0xc0u) != 0x80u) {
                return false;
            }
            scalar = (scalar << 6u) | (continuation & 0x3fu);
        }
        if (scalar < minimum || scalar > 0x10ffffu || (scalar >= 0xd800u && scalar <= 0xdfffu)) {
            return false;
        }
        offset += continuation_count + 1u;
    }
    return true;
}

[[nodiscard]] inline bool valid_error_buffer(const rodaq_error_t* error) noexcept {
    if (error == nullptr) {
        return true;
    }
    if (error->message.data == nullptr) {
        return error->message.capacity == 0u && error->message.size == 0u;
    }
    if (error->message.capacity == 0u || error->message.size > error->message.capacity) {
        return false;
    }
    return valid_utf8(
        std::string_view{reinterpret_cast<const char*>(error->message.data), error->message.size});
}

[[nodiscard]] inline std::string_view error_message(const rodaq_error_t* error) noexcept {
    if (!valid_error_buffer(error) || error == nullptr || error->message.size == 0u) {
        return {};
    }
    return std::string_view{
        reinterpret_cast<const char*>(error->message.data), error->message.size};
}

inline void clear_error(rodaq_error_t* error) noexcept {
    if (error != nullptr) {
        error->message.size = 0u;
    }
}

inline void write_error(rodaq_error_t* error, std::string_view message) noexcept {
    if (error == nullptr || error->message.data == nullptr || error->message.capacity == 0u) {
        return;
    }
    auto size = std::min<std::size_t>(message.size(), error->message.capacity);
    while (size > 0u && !valid_utf8(message.substr(0u, size))) {
        --size;
    }
    std::copy_n(message.data(), size, error->message.data);
    error->message.size = static_cast<std::uint32_t>(size);
}

[[nodiscard]] inline Result<void> result_from_abi_call(
    rodaq_status_code_t code, const rodaq_error_t* error) {
    if (!is_known_status(code)) {
        return Status{StatusCode::Internal, "plugin returned an unknown status"};
    }

    const auto converted = status_code(code);
    if (converted == StatusCode::Ok) {
        if (!valid_error_buffer(error) || (error != nullptr && error->message.size != 0u)) {
            return Status{StatusCode::Internal, "plugin returned an invalid success result"};
        }
        return {};
    }

    const auto message = error_message(error);
    if (message.empty()) {
        return Status{converted, "plugin ABI call failed"};
    }
    return Status{converted, std::string{message}};
}

template <typename Interface>
[[nodiscard]] Result<void> validate_typed_reference(rodaq_interface_reference_t reference) {
    if (reference.context == nullptr || reference.vtable == nullptr) {
        return Status{StatusCode::InvalidArgument, "interface reference is incomplete"};
    }
    if (reference.vtable_size < sizeof(rodaq_struct_header_t)) {
        return Status{StatusCode::InvalidArgument, "interface vtable is too small"};
    }

    const auto* header = reinterpret_cast<const rodaq_struct_header_t*>(reference.vtable);
    if (header->struct_size < sizeof(rodaq_struct_header_t)) {
        return Status{StatusCode::InvalidArgument, "interface vtable header is invalid"};
    }

    const auto readable = std::min<std::size_t>(
        {reference.vtable_size, header->struct_size, sizeof(typename Interface::Vtable)});
    if (readable < Interface::required_prefix_size()) {
        return Status{StatusCode::Unavailable, "interface vtable prefix is incomplete"};
    }
    const auto* typed = reinterpret_cast<const typename Interface::Vtable*>(reference.vtable);
    if (!Interface::required_slots_valid(typed)) {
        return Status{StatusCode::Unavailable, "interface required slot is missing"};
    }
    return {};
}

}  // namespace interface_support

template <typename Interface>
class OperationHandle final {
public:
    OperationHandle() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr && acquire_ != nullptr && release_ != nullptr &&
               interface_support::valid_reference(reference_);
    }

    template <typename Invocation>
    [[nodiscard]] Result<void> invoke(Invocation&& invocation) {
        if (!static_cast<bool>(*this)) {
            return Status{StatusCode::InvalidState, "operation handle is empty"};
        }

        auto acquired = acquire_(owner_.get());
        if (!acquired.ok()) {
            return acquired.status();
        }

        struct Release final {
            void* owner;
            ReleaseFunction release;
            const void* previous_owner;
            ~Release() {
                interface_support::active_operation_owner = previous_owner;
                release(owner);
            }
        } release_guard{owner_.get(), release_, interface_support::active_operation_owner};
        interface_support::active_operation_owner = owner_.get();

        std::array<std::uint8_t, interface_support::kErrorMessageCapacity> error_storage{};
        rodaq_error_t error{};
        error.message.data = error_storage.data();
        error.message.capacity = static_cast<std::uint32_t>(error_storage.size());
        try {
            const auto code = invocation(
                reference_.context,
                // The typed Interface SDK verifies the byte range before restoring
                // the provider's static typed vtable.
                reinterpret_cast<const typename Interface::Vtable*>(reference_.vtable), &error);
            if (!interface_support::is_known_status(code) && report_violation_ != nullptr) {
                report_violation_(
                    owner_.get(), StatusCode::Internal, "operation returned an unknown status");
            }
            return interface_support::result_from_abi_call(code, &error);
        } catch (const std::bad_alloc&) {
            return Status{StatusCode::ResourceExhausted, "plugin operation allocation failed"};
        } catch (...) {
            return Status{StatusCode::Internal, "plugin operation threw an exception"};
        }
    }

private:
    friend class PluginInstance;

    using AcquireFunction = Result<void> (*)(void*) noexcept;
    using ReleaseFunction = void (*)(void*) noexcept;
    using ReportViolationFunction = void (*)(void*, StatusCode, std::string_view) noexcept;

    OperationHandle(
        std::shared_ptr<void> owner, rodaq_interface_reference_t reference, AcquireFunction acquire,
        ReleaseFunction release, ReportViolationFunction report_violation) noexcept
    : owner_(std::move(owner)),
      reference_(reference),
      acquire_(acquire),
      release_(release),
      report_violation_(report_violation) {}

    std::shared_ptr<void> owner_;
    rodaq_interface_reference_t reference_{};
    AcquireFunction acquire_ = nullptr;
    ReleaseFunction release_ = nullptr;
    ReportViolationFunction report_violation_ = nullptr;
};

template <typename Interface>
class ServiceReference final {
public:
    ServiceReference() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return interface_support::valid_reference(reference_);
    }

    template <typename Invocation>
    [[nodiscard]] Result<void> invoke(Invocation&& invocation) {
        if (!static_cast<bool>(*this)) {
            return Status{StatusCode::InvalidState, "service reference is empty"};
        }

        std::array<std::uint8_t, interface_support::kErrorMessageCapacity> error_storage{};
        rodaq_error_t error{};
        error.message.data = error_storage.data();
        error.message.capacity = static_cast<std::uint32_t>(error_storage.size());
        bool call_entered = false;
        if (services_.context != nullptr && services_.vtable != nullptr) {
            const auto* header = reinterpret_cast<const rodaq_struct_header_t*>(services_.vtable);
            const auto required_size = offsetof(rodaq_service_registry_vtable_t, leave_call) +
                                       sizeof(rodaq_service_registry_vtable_t::leave_call);
            if (header != nullptr && header->struct_size >= required_size &&
                services_.vtable->enter_call != nullptr &&
                services_.vtable->leave_call != nullptr) {
                const auto enter_code = services_.vtable->enter_call(services_.context, &error);
                if (enter_code != RODAQ_STATUS_OK) {
                    return interface_support::result_from_abi_call(enter_code, &error);
                }
                call_entered = true;
            }
        }
        struct CallExit final {
            const rodaq_service_registry_reference_t* services;
            bool entered;
            ~CallExit() {
                if (entered) {
                    services->vtable->leave_call(services->context);
                }
            }
        } call_exit{&services_, call_entered};
        try {
            const auto code = invocation(
                reference_.context,
                reinterpret_cast<const typename Interface::Vtable*>(reference_.vtable), &error);
            return interface_support::result_from_abi_call(code, &error);
        } catch (const std::bad_alloc&) {
            return Status{StatusCode::ResourceExhausted, "service allocation failed"};
        } catch (...) {
            return Status{StatusCode::Internal, "service threw an exception"};
        }
    }

private:
    friend class PluginSetup;

    ServiceReference(
        rodaq_interface_reference_t reference, rodaq_service_registry_reference_t services) noexcept
    : reference_(reference), services_(services) {}

    rodaq_interface_reference_t reference_{};
    rodaq_service_registry_reference_t services_{};
};

}  // namespace rodaq::plugin
