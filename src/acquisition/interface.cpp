#include <rodaq/acquisition/interface.hpp>

#include <array>
#include <cstdint>
#include <utility>

namespace rodaq::acquisition {

Handle::Handle(plugin::OperationHandle<Interface> operation) noexcept
: operation_(std::move(operation)) {}

Result<void> Handle::prepare(const PrepareConfig& config, Deadline deadline) {
    if (!abi::valid_prepare(&config)) {
        return Status(StatusCode::InvalidArgument, "Invalid Acquisition prepare input");
    }
    return call([&](auto* context, const auto& table, auto* error) {
        return table.prepare(context, &config, deadline.native(), error);
    });
}

Result<void> Handle::start(Deadline deadline) {
    return call([&](auto* context, const auto& table, auto* error) {
        return table.start(context, deadline.native(), error);
    });
}

Result<void> Handle::quiesce(Deadline deadline) {
    return call([&](auto* context, const auto& table, auto* error) {
        return table.quiesce(context, deadline.native(), error);
    });
}

template <typename Function>
Result<void> Handle::call(Function&& function) {
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

}  // namespace rodaq::acquisition
