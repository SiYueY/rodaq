#ifndef RODAQ_PLUGIN_PLUGIN_HPP_
#define RODAQ_PLUGIN_PLUGIN_HPP_

#include <rodaq/plugin/abi_support.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <exception>
#include <new>
#include <utility>

namespace rodaq::plugin {

template <typename Function>
rodaq_status invoke_void(Function&& function, rodaq_error* out_error) noexcept {
    try {
        Result<void> result = std::forward<Function>(function)();
        if (!result.has_value()) {
            assign_rodaq_error(result.error(), out_error);
            return status_from_error_code(result.error().code());
        }
        return RODAQ_STATUS_OK;
    } catch (const std::bad_alloc&) {
        assign_rodaq_error(
            RODAQ_STATUS_RESOURCE_EXHAUSTED, "sdk", 0, "Plugin SDK operation exhausted memory",
            out_error);
        return RODAQ_STATUS_RESOURCE_EXHAUSTED;
    } catch (const std::exception& error) {
        assign_rodaq_error(RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, error.what(), out_error);
        return RODAQ_STATUS_INTERNAL_ERROR;
    } catch (...) {
        assign_rodaq_error(
            RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, "Plugin SDK caught an unknown exception",
            out_error);
        return RODAQ_STATUS_INTERNAL_ERROR;
    }
}

template <typename Function>
rodaq_status invoke_interface(
    Function&& function, rodaq_interface* out_interface, rodaq_error* out_error) noexcept {
    if (out_interface == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "Plugin interface output is null", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_interface = {};
    try {
        Result<InterfaceView> result = std::forward<Function>(function)();
        if (!result.has_value()) {
            assign_rodaq_error(result.error(), out_error);
            return status_from_error_code(result.error().code());
        }
        if (!result.value().Valid()) {
            assign_rodaq_error(
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "Plugin returned an incomplete interface",
                out_error);
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        *out_interface = result.value().Abi();
        return RODAQ_STATUS_OK;
    } catch (const std::bad_alloc&) {
        assign_rodaq_error(
            RODAQ_STATUS_RESOURCE_EXHAUSTED, "sdk", 0, "Plugin interface query exhausted memory",
            out_error);
        return RODAQ_STATUS_RESOURCE_EXHAUSTED;
    } catch (const std::exception& error) {
        assign_rodaq_error(RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, error.what(), out_error);
        return RODAQ_STATUS_INTERNAL_ERROR;
    } catch (...) {
        assign_rodaq_error(
            RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0,
            "Plugin interface query caught an unknown exception", out_error);
        return RODAQ_STATUS_INTERNAL_ERROR;
    }
}

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_PLUGIN_HPP_
