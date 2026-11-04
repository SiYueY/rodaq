#ifndef RODAQ_PLUGIN_PLUGIN_INSTANCE_HPP_
#define RODAQ_PLUGIN_PLUGIN_INSTANCE_HPP_

#include "plugin/plugin_library.hpp"

#include <memory>
#include <string_view>

namespace rodaq::plugin {

class PluginInstanceState;

class PluginInstanceLease final {
public:
    PluginInstanceLease() = default;
    PluginInstanceLease(const PluginInstanceLease&) = default;
    PluginInstanceLease(PluginInstanceLease&&) noexcept = default;
    PluginInstanceLease& operator=(const PluginInstanceLease&) = default;
    PluginInstanceLease& operator=(PluginInstanceLease&&) noexcept = default;
    ~PluginInstanceLease() = default;

    bool valid() const noexcept;
    void quarantine() noexcept;
    std::shared_ptr<void> lifetime_owner() const noexcept;

private:
    explicit PluginInstanceLease(std::shared_ptr<PluginInstanceState> state) noexcept;

    std::shared_ptr<PluginInstanceState> state_;

    friend class AcquisitionPluginInstance;
    friend class StoragePluginInstance;
};

class PluginInstanceStateAccess {
public:
    using BindFunction =
        rodaq_status (*)(void*, const rodaq_service_registry*, rodaq_error*) noexcept;
    using ConfigureFunction = rodaq_status (*)(void*, rodaq_config_view, rodaq_error*) noexcept;
    using QueryInterfaceFunction =
        rodaq_status (*)(void*, rodaq_string_view, rodaq_interface*, rodaq_error*) noexcept;
    using StartFunction = rodaq_status (*)(void*, rodaq_error*) noexcept;
    using StopFunction = rodaq_status (*)(void*, rodaq_error*) noexcept;
    using DestroyFunction = void (*)(void*) noexcept;
    using OpenFunction =
        rodaq_status (*)(void*, const rodaq_storage_open_request*, rodaq_error*) noexcept;
    using SubmitFunction = rodaq_status (*)(
        void*, const rodaq_storage_submission*, rodaq_storage_submit_result*,
        rodaq_error*) noexcept;
    using FlushFunction = rodaq_status (*)(void*, rodaq_error*) noexcept;
    using CloseFunction =
        rodaq_status (*)(void*, const rodaq_storage_close_request*, rodaq_error*) noexcept;

    static std::shared_ptr<PluginInstanceState> create(
        const PluginLibraryLease& library_lease, void* instance, void (*destroy)(void*) noexcept,
        BindFunction bind_function, ConfigureFunction configure_function,
        QueryInterfaceFunction query_interface_function, StartFunction start_function,
        StopFunction stop_function, OpenFunction open_function, SubmitFunction submit_function,
        FlushFunction flush_function, CloseFunction close_function);

    static Result<void> bind(
        const std::shared_ptr<PluginInstanceState>& state, const rodaq_service_registry& services);

    static Result<void> configure(
        const std::shared_ptr<PluginInstanceState>& state, rodaq_config_view config);

    static Result<rodaq_interface> query_interface(
        const std::shared_ptr<PluginInstanceState>& state, std::string_view interface_id);

    static Result<void> start(const std::shared_ptr<PluginInstanceState>& state);
    static Result<void> stop(const std::shared_ptr<PluginInstanceState>& state);
    static void destroy(const std::shared_ptr<PluginInstanceState>& state) noexcept;
    static Result<void> open(
        const std::shared_ptr<PluginInstanceState>& state,
        const rodaq_storage_open_request& request);
    static Result<rodaq_storage_submit_result> submit(
        const std::shared_ptr<PluginInstanceState>& state,
        const rodaq_storage_submission& submission);
    static Result<void> flush(const std::shared_ptr<PluginInstanceState>& state);
    static Result<void> close(
        const std::shared_ptr<PluginInstanceState>& state,
        const rodaq_storage_close_request& request);

    static void set_required_control_interface(
        const std::shared_ptr<PluginInstanceState>& state, rodaq_interface interface) noexcept;

    static Result<rodaq_interface> required_control_interface(
        const std::shared_ptr<PluginInstanceState>& state);
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_PLUGIN_INSTANCE_HPP_
