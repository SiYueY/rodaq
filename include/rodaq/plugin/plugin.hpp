#ifndef RODAQ_PLUGIN_PLUGIN_HPP_
#define RODAQ_PLUGIN_PLUGIN_HPP_

#include <rodaq/plugin/abi_support.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <exception>
#include <cstring>
#include <new>
#include <utility>

namespace rodaq::plugin {

template <typename Struct>
std::uint32_t clear_output_prefix(Struct* output, std::uint32_t capacity) noexcept {
    const std::uint32_t initialized = rodaq::initialized_prefix_size<Struct>(capacity);
    if (output != nullptr && initialized != 0) {
        std::memset(output, 0, initialized);
    }
    return initialized;
}

template <typename Function>
rodaq_status_code invoke_void(Function&& function, rodaq_status* out_status) noexcept {
    try {
        Result<void> result = std::forward<Function>(function)();
        if (!result.ok()) {
            assign_abi_status(result.status(), out_status);
            return abi_status_from_status_code(result.status().code());
        }
        return RODAQ_STATUS_OK;
    } catch (const std::bad_alloc&) {
        assign_abi_status(
            RODAQ_STATUS_RESOURCE_EXHAUSTED, "sdk", 0, "Plugin SDK operation exhausted memory",
            out_status);
        return RODAQ_STATUS_RESOURCE_EXHAUSTED;
    } catch (const std::exception& error) {
        assign_abi_status(RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, error.what(), out_status);
        return RODAQ_STATUS_INTERNAL_ERROR;
    } catch (...) {
        assign_abi_status(
            RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, "Plugin SDK caught an unknown exception",
            out_status);
        return RODAQ_STATUS_INTERNAL_ERROR;
    }
}

template <typename Function>
rodaq_status_code invoke_interface(
    Function&& function, rodaq_interface* out_interface, rodaq_status* out_status) noexcept {
    if (out_interface == nullptr) {
        assign_abi_status(
            RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "Plugin interface output is null", out_status);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_interface = {};
    try {
        Result<InterfaceRef> result = std::forward<Function>(function)();
        if (!result.ok()) {
            assign_abi_status(result.status(), out_status);
            return abi_status_from_status_code(result.status().code());
        }
        if (!result.value().valid()) {
            assign_abi_status(
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "Plugin returned an incomplete interface",
                out_status);
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        *out_interface = result.value().abi();
        return RODAQ_STATUS_OK;
    } catch (const std::bad_alloc&) {
        assign_abi_status(
            RODAQ_STATUS_RESOURCE_EXHAUSTED, "sdk", 0, "Plugin interface query exhausted memory",
            out_status);
        return RODAQ_STATUS_RESOURCE_EXHAUSTED;
    } catch (const std::exception& error) {
        assign_abi_status(RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, error.what(), out_status);
        return RODAQ_STATUS_INTERNAL_ERROR;
    } catch (...) {
        assign_abi_status(
            RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0,
            "Plugin interface query caught an unknown exception", out_status);
        return RODAQ_STATUS_INTERNAL_ERROR;
    }
}

rodaq_status_code invalid_input(const char* message, rodaq_status* out_status) noexcept;

template <typename Output, typename Function, typename Writer>
rodaq_status_code invoke_struct_output(
    Output* output, std::uint32_t minimum_size, Function&& function, Writer&& writer,
    rodaq_status* out_status) noexcept {
    if (output == nullptr) {
        return invalid_input("struct output is null", out_status);
    }
    const std::uint32_t capacity = output->struct_size;
    if (capacity < minimum_size) {
        return invalid_input(
            "struct output capacity is below the minimum supported size", out_status);
    }
    try {
        auto result = std::forward<Function>(function)();
        if (!result.ok()) {
            assign_abi_status(result.status(), out_status);
            return abi_status_from_status_code(result.status().code());
        }
        std::forward<Writer>(writer)(output, capacity, result.value());
        return RODAQ_STATUS_OK;
    } catch (const std::bad_alloc&) {
        assign_abi_status(
            RODAQ_STATUS_RESOURCE_EXHAUSTED, "sdk", 0,
            "Plugin SDK output operation exhausted memory", out_status);
        return RODAQ_STATUS_RESOURCE_EXHAUSTED;
    } catch (const std::exception& error) {
        assign_abi_status(RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, error.what(), out_status);
        return RODAQ_STATUS_INTERNAL_ERROR;
    } catch (...) {
        assign_abi_status(
            RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0,
            "Plugin SDK output operation caught an unknown exception", out_status);
        return RODAQ_STATUS_INTERNAL_ERROR;
    }
}

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_PLUGIN_HPP_
