#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <rodaq/plugin/authoring.hpp>

struct LifecycleProbeVtable {
    rodaq_struct_header_t header;
    rodaq_status_code_t (*invoke)(
        rodaq_interface_context_t*, std::int64_t, std::int64_t*, rodaq_error_t*);
};

template <typename Implementation>
struct LifecycleProbeProvider {
    static rodaq_status_code_t invoke(
        rodaq_interface_context_t* context, std::int64_t event, std::int64_t* value,
        rodaq_error_t* error) noexcept {
        if (value != nullptr) {
            *value = 0;
        }
        return rodaq::plugin::authoring::exception_boundary(error, [&]() -> rodaq::Result<void> {
            if (context == nullptr || value == nullptr) {
                return rodaq::Status(rodaq::StatusCode::InvalidArgument, "Invalid probe input");
            }
            auto result = reinterpret_cast<Implementation*>(context)->invoke(event);
            if (!result.ok()) {
                return result.status();
            }
            *value = result.value();
            return {};
        });
    }

    static const LifecycleProbeVtable* vtable() noexcept {
        static const LifecycleProbeVtable table{{sizeof(table)}, invoke};
        return &table;
    }
};

class LifecycleProbeReference;

struct LifecycleProbeService {
    using Vtable = LifecycleProbeVtable;
    using Reference = LifecycleProbeReference;
    static constexpr std::string_view kInterfaceId = "com.example.lifecycle.probe";
    static constexpr std::uint16_t kMajor = 1;
    static constexpr std::uint16_t kMinor = 0;

    template <typename Implementation>
    using Provider = LifecycleProbeProvider<Implementation>;

    static constexpr std::size_t minimum_required_prefix_size() noexcept {
        return offsetof(Vtable, invoke) + sizeof(Vtable::invoke);
    }

    static bool valid_vtable(const Vtable& table, std::uint16_t) noexcept {
        return table.invoke != nullptr;
    }
};

class LifecycleProbeReference {
public:
    explicit LifecycleProbeReference(
        rodaq::plugin::ServiceReference<LifecycleProbeService> reference)
    : reference_(std::move(reference)) {}

    rodaq::Result<std::int64_t> invoke(std::int64_t event) const {
        return reference_.invoke(
            [&](auto* context, const auto& table) -> rodaq::Result<std::int64_t> {
                std::array<std::uint8_t, 128> buffer{};
                rodaq_error_t error{{buffer.data(), buffer.size(), 0}};
                const rodaq::plugin::abi::BufferSnapshot expected{buffer.data(), buffer.size()};
                std::int64_t value = 0;
                const auto code = table.invoke(context, event, &value, &error);
                auto status = rodaq::plugin::abi::status_from_abi(code, error, expected);
                if (!status.ok()) {
                    return status;
                }
                return value;
            });
    }

private:
    rodaq::plugin::ServiceReference<LifecycleProbeService> reference_;
};
