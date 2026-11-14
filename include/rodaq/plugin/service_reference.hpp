#pragma once

#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <rodaq/plugin/provider.hpp>

namespace rodaq::plugin {

class PluginSetup;

/** Borrowed service capability; never extends Registry/provider lifetime. */
template <typename Interface>
class ServiceReference {
public:
    template <typename Function>
    auto invoke(Function&& function) const {
        using Return = std::invoke_result_t<
            Function, rodaq_interface_context_t*, const typename Interface::Vtable&>;
        const auto* registry = registry_.vtable;
        const bool managed = abi::readable_slot(
            registry->header.struct_size, offsetof(rodaq_service_registry_vtable_t, leave_call),
            sizeof(registry->leave_call));
        if (!managed) {
            // Older Registry supports only synchronous calls on the native call stack.
            return function(context_, table_);
        }
        std::array<std::uint8_t, 512> buffer{};
        rodaq_error_t error{{buffer.data(), buffer.size(), 0}};
        const abi::BufferSnapshot expected{buffer.data(), buffer.size()};
        const auto code = registry->enter_call(registry_.context, &error);
        if (code != RODAQ_STATUS_OK || !abi::valid_error(code, error, expected)) {
            // Even malformed OK granted a permit: release it before rejecting output.
            if (code == RODAQ_STATUS_OK) {
                registry->leave_call(registry_.context);
            }
            return Return(abi::status_from_abi(code, error, expected));
        }
        struct Permit {
            rodaq_service_registry_reference_t registry;

            ~Permit() { registry.vtable->leave_call(registry.context); }
        } permit{registry_};
        return function(context_, table_);
    }

private:
    friend class PluginSetup;

    ServiceReference(
        rodaq_service_registry_reference_t registry, const rodaq_interface_reference_t& reference,
        typename Interface::Vtable table) noexcept
    : registry_(registry), context_(reference.context), table_(table) {}

    rodaq_service_registry_reference_t registry_;
    rodaq_interface_context_t* context_;
    typename Interface::Vtable table_;
};

}  // namespace rodaq::plugin
