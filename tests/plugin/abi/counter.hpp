#pragma once

#include "counter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <rodaq/plugin/authoring.hpp>
#include <rodaq/plugin/operation.hpp>

class CounterHandle;

template <typename Implementation>
struct CounterProvider {
    static rodaq_status_code_t increment(
        rodaq_interface_context_t* context, std::int64_t delta, std::int64_t* value,
        rodaq_error_t* error) noexcept {
        if (value != nullptr) {
            *value = 0;
        }
        return rodaq::plugin::authoring::exception_boundary(error, [&]() -> rodaq::Result<void> {
            if (context == nullptr || value == nullptr) {
                return rodaq::Status(rodaq::StatusCode::InvalidArgument, "Invalid Counter input");
            }
            auto result = reinterpret_cast<Implementation*>(context)->increment(delta);
            if (!result.ok()) {
                return result.status();
            }
            *value = result.value();
            return {};
        });
    }

    static rodaq_status_code_t current(
        rodaq_interface_context_t* context, std::int64_t* value, rodaq_error_t* error) noexcept {
        if (value != nullptr) {
            *value = 0;
        }
        return rodaq::plugin::authoring::exception_boundary(error, [&]() -> rodaq::Result<void> {
            if (context == nullptr || value == nullptr) {
                return rodaq::Status(rodaq::StatusCode::InvalidArgument, "Invalid Counter input");
            }
            auto result = reinterpret_cast<const Implementation*>(context)->current();
            if (!result.ok()) {
                return result.status();
            }
            *value = result.value();
            return {};
        });
    }

    static const rodaq_counter_vtable_t* vtable() noexcept {
        static const rodaq_counter_vtable_t table{{sizeof(table)}, increment, current};
        return &table;
    }
};

struct CounterInterface {
    using Vtable = rodaq_counter_vtable_t;
    using Handle = CounterHandle;
    static constexpr std::string_view kInterfaceId = RODAQ_COUNTER_INTERFACE_ID;
    static constexpr std::uint16_t kMajor = 1;
    static constexpr std::uint16_t kMinor = 0;

    template <typename Implementation>
    using Provider = CounterProvider<Implementation>;

    static constexpr std::size_t minimum_required_prefix_size() noexcept {
        return offsetof(Vtable, current) + sizeof(Vtable::current);
    }

    static bool valid_vtable(const Vtable& table, std::uint16_t) noexcept {
        return table.increment != nullptr && table.current != nullptr;
    }
};

class CounterHandle {
public:
    explicit CounterHandle(rodaq::plugin::OperationHandle<CounterInterface> operation)
    : operation_(std::move(operation)) {}

    rodaq::Result<std::int64_t> increment(std::int64_t delta) {
        return read([&](auto* context, const auto& table, auto* value, auto* error) {
            return table.increment(context, delta, value, error);
        });
    }

    rodaq::Result<std::int64_t> current() const {
        return read([](auto* context, const auto& table, auto* value, auto* error) {
            return table.current(context, value, error);
        });
    }

private:
    template <typename Function>
    rodaq::Result<std::int64_t> read(Function&& function) const {
        return operation_.invoke(
            [&](auto* context, const auto& table) -> rodaq::Result<std::int64_t> {
                std::array<std::uint8_t, 128> buffer{};
                rodaq_error_t error{{buffer.data(), buffer.size(), 0}};
                const rodaq::plugin::abi::BufferSnapshot expected{buffer.data(), buffer.size()};
                std::int64_t value = 0;
                rodaq_status_code_t code = RODAQ_STATUS_INTERNAL;
                try {
                    code = function(context, table, &value, &error);
                } catch (...) {
                    return rodaq::Status(rodaq::StatusCode::Internal, "Counter ABI exception");
                }
                auto status = rodaq::plugin::abi::status_from_abi(code, error, expected);
                if (!status.ok()) {
                    return status;
                }
                return value;
            });
    }

    rodaq::plugin::OperationHandle<CounterInterface> operation_;
};
