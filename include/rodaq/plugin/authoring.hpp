#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <vector>

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/setup.hpp>

namespace rodaq::plugin {

class PluginBase {
public:
    virtual ~PluginBase() noexcept = default;

    PluginBase(const PluginBase&) = delete;
    PluginBase& operator=(const PluginBase&) = delete;

    virtual Result<void> configure(PluginSetup& setup) = 0;
    virtual Result<void> shutdown(Deadline deadline) = 0;

protected:
    PluginBase() = default;
};

namespace authoring {

template <typename Plugin>
struct Instance {
    static_assert(std::is_base_of_v<PluginBase, Plugin>, "Plugin must implement PluginBase");
    static_assert(std::is_default_constructible_v<Plugin>, "Plugin must be default constructible");
    static_assert(std::is_nothrow_destructible_v<Plugin>, "Plugin destructor must be noexcept");

    Plugin plugin;
    std::vector<InterfaceEntry> entries;
    bool configure_started{false};
    bool configured{false};

    static rodaq_status_code_t configure(
        rodaq_plugin_context_t* context, const rodaq_application_callbacks_t* application,
        const rodaq_config_t* config, rodaq_error_t* error) noexcept {
        return exception_boundary(error, [&]() -> Result<void> {
            if (context == nullptr || !valid_application_callbacks(application) ||
                config == nullptr ||
                config->header.struct_size <
                    offsetof(rodaq_config_t, json) + sizeof(config->json) ||
                !abi::valid_string(config->json)) {
                return Status(StatusCode::InvalidArgument, "Invalid configure input");
            }
            auto& state = *reinterpret_cast<Instance*>(context);
            if (state.configure_started) {
                return Status(StatusCode::InvalidState, "Configure runs once");
            }
            state.configure_started = true;
            state.entries.reserve(kMaxInterfaces);
            const auto json = config->json.size == 0
                                  ? std::string_view{}
                                  : std::string_view(config->json.data, config->json.size);
            PluginSetup setup(json, *application, state.entries);
            auto result = state.plugin.configure(setup);
            if (!result.ok()) {
                state.entries.clear();
                return result;
            }
            state.configured = true;
            return {};
        });
    }

    static rodaq_status_code_t query(
        rodaq_plugin_context_t* context, const rodaq_interface_query_t* request,
        rodaq_interface_reference_t* reference, rodaq_error_t* error) noexcept {
        abi::write_error(error, {});
        if (reference == nullptr) {
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        *reference = {};
        if (!abi::valid_error_input(error) || context == nullptr) {
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        const auto& state = *reinterpret_cast<Instance*>(context);
        if (!state.configured) {
            return RODAQ_STATUS_INVALID_STATE;
        }
        return query_interface(state.entries, request, reference, error);
    }

    static rodaq_status_code_t shutdown(
        rodaq_plugin_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error) noexcept {
        return exception_boundary(error, [&]() -> Result<void> {
            if (context == nullptr) {
                return Status(StatusCode::InvalidArgument, "Missing plugin context");
            }
            auto value = Deadline::from_native(deadline);
            if (!value.ok()) {
                return value.status();
            }
            return reinterpret_cast<Instance*>(context)->plugin.shutdown(value.value());
        });
    }

    static void destroy(rodaq_plugin_context_t* context) noexcept {
        delete reinterpret_cast<Instance*>(context);
    }

    inline static const rodaq_plugin_instance_vtable_t kVtable{
        {sizeof(rodaq_plugin_instance_vtable_t)}, configure, query, shutdown, destroy};

    static rodaq_status_code_t create(
        rodaq_plugin_instance_t* instance, rodaq_error_t* error) noexcept {
        if (instance != nullptr) {
            *instance = {};
        }
        return exception_boundary(error, [&]() -> Result<void> {
            if (instance == nullptr) {
                return Status(StatusCode::InvalidArgument, "Missing instance output");
            }
            auto state = std::make_unique<Instance>();
            instance->context = reinterpret_cast<rodaq_plugin_context_t*>(state.release());
            instance->vtable = &kVtable;
            return {};
        });
    }
};

}  // namespace authoring
}  // namespace rodaq::plugin

/** One invocation per plugin translation unit; metadata comes from rodaq_register_plugin. */
#define RODAQ_DEFINE_PLUGIN(Plugin)                                                   \
    extern "C" rodaq_status_code_t rodaq_plugin_create_instance(                      \
        rodaq_plugin_instance_t* instance, rodaq_error_t* error) {                    \
        return ::rodaq::plugin::authoring::Instance<Plugin>::create(instance, error); \
    }
