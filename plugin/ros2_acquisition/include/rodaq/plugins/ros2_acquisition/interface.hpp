#pragma once

#include <rodaq/plugin/interface.hpp>

#include "rodaq/plugins/ros2_acquisition/c/interface.h"

#include <cstdint>
#include <string_view>

namespace rodaq::plugins::ros2_acquisition {

using rodaq::plugin::OperationHandle;
using rodaq::plugin::ServiceReference;

struct ControlInterface {
    using Vtable = rodaq_reference_ros2_control_vtable_t;
    using Handle = OperationHandle<ControlInterface>;

    static constexpr std::string_view kInterfaceId = "com.rodaq.reference.ros2.control";
    static constexpr std::uint16_t kMajor = 1;
    static constexpr std::uint16_t kMinor = 0;
    static constexpr std::uint16_t kMinMinor = 0;

    template <typename Implementation>
    class Provider final {
    public:
        static const Vtable* vtable() noexcept {
            static const Vtable value{rodaq_struct_header_t{sizeof(Vtable)}, &start, &stop};
            return &value;
        }

    private:
        static rodaq_status_code_t start(
            rodaq_reference_ros2_control_context_t* context, rodaq_error_t*) noexcept {
            try {
                return reinterpret_cast<Implementation*>(context)->start();
            } catch (...) {
                return RODAQ_STATUS_INTERNAL;
            }
        }

        static rodaq_status_code_t stop(
            rodaq_reference_ros2_control_context_t* context, rodaq_error_t*) noexcept {
            try {
                reinterpret_cast<Implementation*>(context)->stop();
                return RODAQ_STATUS_OK;
            } catch (...) {
                return RODAQ_STATUS_INTERNAL;
            }
        }
    };

    static constexpr std::size_t required_prefix_size() noexcept { return sizeof(Vtable); }

    static bool required_slots_valid(const Vtable* vtable) noexcept {
        return vtable != nullptr && vtable->start != nullptr && vtable->stop != nullptr;
    }
};

struct SinkInterface {
    using Vtable = rodaq_reference_ros2_sink_vtable_t;
    using Reference = ServiceReference<SinkInterface>;

    static constexpr std::string_view kInterfaceId = "com.rodaq.reference.ros2.sink";
    static constexpr std::uint16_t kMajor = 1;
    static constexpr std::uint16_t kMinor = 0;
    static constexpr std::uint16_t kMinMinor = 0;

    template <typename Implementation>
    class Provider final {
    public:
        static const Vtable* vtable() noexcept {
            static const Vtable value{rodaq_struct_header_t{sizeof(Vtable)}, &record};
            return &value;
        }

    private:
        static rodaq_status_code_t record(
            rodaq_reference_ros2_sink_context_t* context,
            const rodaq_reference_ros2_sample_t* sample, rodaq_error_t*) noexcept {
            if (context == nullptr || sample == nullptr) {
                return RODAQ_STATUS_INVALID_ARGUMENT;
            }
            try {
                return reinterpret_cast<Implementation*>(context)->record(*sample);
            } catch (...) {
                return RODAQ_STATUS_INTERNAL;
            }
        }
    };

    static constexpr std::size_t required_prefix_size() noexcept { return sizeof(Vtable); }

    static bool required_slots_valid(const Vtable* vtable) noexcept {
        return vtable != nullptr && vtable->record != nullptr;
    }
};

inline Result<void> start(ControlInterface::Handle& handle) {
    return handle.invoke([](rodaq_interface_context_t* context,
                            const ControlInterface::Vtable* vtable, rodaq_error_t* error) {
        return vtable->start(
            reinterpret_cast<rodaq_reference_ros2_control_context_t*>(context), error);
    });
}

inline Result<void> stop(ControlInterface::Handle& handle) {
    return handle.invoke([](rodaq_interface_context_t* context,
                            const ControlInterface::Vtable* vtable, rodaq_error_t* error) {
        return vtable->stop(
            reinterpret_cast<rodaq_reference_ros2_control_context_t*>(context), error);
    });
}

}  // namespace rodaq::plugins::ros2_acquisition
