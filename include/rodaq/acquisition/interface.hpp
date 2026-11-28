#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <rodaq/c/acquisition.h>
#include <rodaq/acquisition/provider.hpp>
#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/operation.hpp>

namespace rodaq::acquisition {

using plugin::Deadline;
class Handle;

struct Interface {
    using Vtable = rodaq_acquisition_interface_vtable_t;
    using Handle = acquisition::Handle;
    static constexpr std::string_view kInterfaceId = RODAQ_ACQUISITION_INTERFACE_ID;
    static constexpr std::uint16_t kMajor = RODAQ_ACQUISITION_INTERFACE_MAJOR;
    static constexpr std::uint16_t kMinor = RODAQ_ACQUISITION_INTERFACE_MINOR;

    template <typename Implementation>
    using Provider = acquisition::Provider<Implementation>;

    static constexpr std::size_t minimum_required_prefix_size() noexcept {
        return offsetof(Vtable, quiesce) + sizeof(Vtable::quiesce);
    }

    static bool valid_vtable(const Vtable& table, std::uint16_t) noexcept {
        return table.prepare != nullptr && table.start != nullptr && table.quiesce != nullptr;
    }
};

/**
 * Header-only managed wrapper (keeps domain ABI symbols out of librodaq_plugin.so).
 * No mutable business state: the backend and Core retain their own authorities.
 */
class Handle {
public:
    explicit Handle(plugin::OperationHandle<Interface> operation) noexcept
    : operation_(std::move(operation)) {}

    Result<void> prepare(
        const PrepareConfig& config, const CallbacksTable& callbacks, Deadline deadline) {
        if (!abi::valid_prepare(&config) || !abi::valid_callbacks(&callbacks)) {
            return Status(StatusCode::InvalidArgument, "Invalid Acquisition prepare input");
        }
        return call([&](auto* context, const auto& table, auto* error) {
            return table.prepare(context, &config, &callbacks, deadline.native(), error);
        });
    }

    Result<void> start(Deadline deadline) {
        return call([&](auto* context, const auto& table, auto* error) {
            return table.start(context, deadline.native(), error);
        });
    }

    Result<void> quiesce(Deadline deadline) {
        return call([&](auto* context, const auto& table, auto* error) {
            return table.quiesce(context, deadline.native(), error);
        });
    }

private:
    template <typename Function>
    Result<void> call(Function&& function) {
        return operation_.invoke([&](auto* context, const auto& table) -> Result<void> {
            std::array<std::uint8_t, 512> bytes{};
            rodaq_error_t error{{bytes.data(), bytes.size(), 0}};
            const plugin::abi::BufferSnapshot expected{bytes.data(), bytes.size()};
            try {
                const auto code = function(context, table, &error);
                if (!plugin::abi::known_status(code) ||
                    !plugin::abi::valid_error(code, error, expected)) {
                    operation_.report_abi_violation("Acquisition status/error contract violation");
                }
                auto status = plugin::abi::status_from_abi(code, error, expected);
                return status.ok() ? Result<void>{} : Result<void>{std::move(status)};
            } catch (...) {
                operation_.report_abi_violation("Exception crossed Acquisition operation ABI");
                return Status(StatusCode::Internal, "Exception crossed Acquisition operation ABI");
            }
        });
    }

    plugin::OperationHandle<Interface> operation_;
};

}  // namespace rodaq::acquisition
