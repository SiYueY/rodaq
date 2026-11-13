#pragma once

#include <rodaq/plugin/c/abi.h>
#include <rodaq/plugin/interface.hpp>

#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>

namespace rodaq::plugin::test {

typedef struct counter_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*increment)(
        rodaq_interface_context_t* context, int64_t delta, int64_t* value, rodaq_error_t* error);

    rodaq_status_code_t (*current)(
        rodaq_interface_context_t* context, int64_t* value, rodaq_error_t* error);
} counter_vtable_t;

struct CounterInterface {
    using Vtable = counter_vtable_t;
    using Handle = OperationHandle<CounterInterface>;

    static constexpr std::string_view kInterfaceId = "com.rodaq.test.counter";
    static constexpr std::uint16_t kMajor = 1;
    static constexpr std::uint16_t kMinor = 0;
    static constexpr std::uint16_t kMinMinor = 0;

    template <typename Implementation>
    class Provider final {
    public:
        static const Vtable* vtable() noexcept {
            static const Vtable value{rodaq_struct_header_t{sizeof(Vtable)}, &increment, &current};
            return &value;
        }

    private:
        static rodaq_status_code_t increment(
            rodaq_interface_context_t* context, int64_t delta, int64_t* value,
            rodaq_error_t* error) noexcept {
            if (value != nullptr) {
                *value = 0;
            }
            if (context == nullptr || value == nullptr || !valid_error(error)) {
                return RODAQ_STATUS_INVALID_ARGUMENT;
            }
            clear_error(error);
            try {
                auto& implementation =
                    *static_cast<Implementation*>(reinterpret_cast<Implementation*>(context));
                *value = implementation.increment(delta);
                return RODAQ_STATUS_OK;
            } catch (const std::bad_alloc&) {
                return RODAQ_STATUS_RESOURCE_EXHAUSTED;
            } catch (...) {
                return RODAQ_STATUS_INTERNAL;
            }
        }

        static rodaq_status_code_t current(
            rodaq_interface_context_t* context, int64_t* value, rodaq_error_t* error) noexcept {
            if (value != nullptr) {
                *value = 0;
            }
            if (context == nullptr || value == nullptr || !valid_error(error)) {
                return RODAQ_STATUS_INVALID_ARGUMENT;
            }
            clear_error(error);
            try {
                auto& implementation =
                    *static_cast<Implementation*>(reinterpret_cast<Implementation*>(context));
                *value = implementation.current();
                return RODAQ_STATUS_OK;
            } catch (const std::bad_alloc&) {
                return RODAQ_STATUS_RESOURCE_EXHAUSTED;
            } catch (...) {
                return RODAQ_STATUS_INTERNAL;
            }
        }

        static bool valid_error(const rodaq_error_t* error) noexcept {
            return interface_support::valid_error_buffer(error);
        }

        static void clear_error(rodaq_error_t* error) noexcept {
            interface_support::clear_error(error);
        }
    };

    static constexpr std::size_t required_prefix_size() noexcept { return sizeof(Vtable); }

    static bool required_slots_valid(const Vtable* vtable) noexcept {
        return vtable != nullptr && vtable->increment != nullptr && vtable->current != nullptr;
    }
};

typedef struct counter_sink_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*record)(
        rodaq_interface_context_t* context, int64_t value, rodaq_error_t* error);
} counter_sink_vtable_t;

struct CounterSinkInterface {
    using Vtable = counter_sink_vtable_t;
    using Reference = ServiceReference<CounterSinkInterface>;

    static constexpr std::string_view kInterfaceId = "com.rodaq.test.counter-sink";
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
            rodaq_interface_context_t* context, int64_t value, rodaq_error_t* error) noexcept {
            if (context == nullptr) {
                return RODAQ_STATUS_INVALID_ARGUMENT;
            }
            if (!interface_support::valid_error_buffer(error)) {
                return RODAQ_STATUS_INVALID_ARGUMENT;
            }
            interface_support::clear_error(error);
            try {
                auto& implementation = *reinterpret_cast<Implementation*>(context);
                implementation.record(value);
                return RODAQ_STATUS_OK;
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

inline Result<int64_t> increment(CounterInterface::Handle& handle, int64_t delta) {
    int64_t value = 0;
    auto result = handle.invoke(
        [&](rodaq_interface_context_t* context, const CounterInterface::Vtable* vtable,
            rodaq_error_t* error) { return vtable->increment(context, delta, &value, error); });
    if (!result.ok()) {
        return result.status();
    }
    return value;
}

inline Result<int64_t> current(CounterInterface::Handle& handle) {
    int64_t value = 0;
    auto result = handle.invoke(
        [&](rodaq_interface_context_t* context, const CounterInterface::Vtable* vtable,
            rodaq_error_t* error) { return vtable->current(context, &value, error); });
    if (!result.ok()) {
        return result.status();
    }
    return value;
}

inline Result<void> record(CounterSinkInterface::Reference& reference, int64_t value) {
    return reference.invoke(
        [&](rodaq_interface_context_t* context, const CounterSinkInterface::Vtable* vtable,
            rodaq_error_t* error) { return vtable->record(context, value, error); });
}

}  // namespace rodaq::plugin::test
