#ifndef RODAQ_PLUGIN_ACQUISITION_PLUGIN_HPP_
#define RODAQ_PLUGIN_ACQUISITION_PLUGIN_HPP_

#include <rodaq/plugin/abi/acquisition.h>
#include <rodaq/plugin/config_view.hpp>
#include <rodaq/plugin/plugin.hpp>
#include <rodaq/plugin/services.hpp>

#include <string_view>

namespace rodaq::plugin {

class AcquisitionPlugin {
public:
    virtual ~AcquisitionPlugin() = default;
    virtual Result<void> bind(AcquisitionContext context) = 0;
    virtual Result<void> configure(const ConfigView& config) = 0;
    virtual Result<void> start() = 0;
    virtual Result<void> stop() = 0;
    virtual Result<void> prepare_binding_switch(
        rodaq_binding_transition transition, rodaq_stream_handle stream,
        rodaq_stream_descriptor_handle old_descriptor,
        rodaq_stream_descriptor_handle new_descriptor) = 0;
    virtual Result<void> commit_binding_switch(rodaq_binding_transition transition) = 0;
    virtual Result<void> abort_binding_switch(
        rodaq_binding_transition transition, rodaq_binding_abort_disposition disposition) = 0;
    virtual Result<void> quiesce_stream(rodaq_stream_handle stream) = 0;
    virtual Result<void> quiesce_plugin() = 0;
    virtual Result<InterfaceView> query_interface(std::string_view interface_id);
};

}  // namespace rodaq::plugin

#define RODAQ_EXPORT_ACQUISITION_PLUGIN(PluginType, Descriptor)                                  \
    namespace {                                                                                  \
    rodaq_status_code rodaq_sdk_acquisition_create(                                              \
        void** out_instance, rodaq_status* out_status) noexcept {                                \
        if (out_instance == nullptr) {                                                           \
            rodaq::assign_abi_status(                                                            \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "create output is null", out_status);   \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                \
        }                                                                                        \
        *out_instance = nullptr;                                                                 \
        try {                                                                                    \
            *out_instance = new PluginType();                                                    \
            return RODAQ_STATUS_OK;                                                              \
        } catch (const std::bad_alloc&) {                                                        \
            rodaq::assign_abi_status(                                                            \
                RODAQ_STATUS_RESOURCE_EXHAUSTED, "sdk", 0, "Plugin allocation failed",           \
                out_status);                                                                     \
            return RODAQ_STATUS_RESOURCE_EXHAUSTED;                                              \
        } catch (const std::exception& error) {                                                  \
            rodaq::assign_abi_status(                                                            \
                RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, error.what(), out_status);                \
            return RODAQ_STATUS_INTERNAL_ERROR;                                                  \
        } catch (...) {                                                                          \
            rodaq::assign_abi_status(                                                            \
                RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, "Unknown create exception", out_status);  \
            return RODAQ_STATUS_INTERNAL_ERROR;                                                  \
        }                                                                                        \
    }                                                                                            \
    void rodaq_sdk_acquisition_destroy(void* instance) noexcept {                                \
        delete static_cast<PluginType*>(instance);                                               \
    }                                                                                            \
    rodaq_status_code rodaq_sdk_acquisition_bind(                                                \
        void* instance, const rodaq_service_registry* services,                                  \
        rodaq_status* out_status) noexcept {                                                     \
        if (instance == nullptr || services == nullptr) {                                        \
            rodaq::assign_abi_status(                                                            \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "bind input is null", out_status);      \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                \
        }                                                                                        \
        return rodaq::plugin::invoke_void(                                                       \
            [instance, services]() -> rodaq::Result<void> {                                      \
                auto registry = rodaq::plugin::ServiceRegistryView::from_abi(*services);         \
                if (!registry.ok()) {                                                            \
                    return registry.status();                                                    \
                }                                                                                \
                auto context = rodaq::plugin::AcquisitionContext::bind(registry.value());        \
                if (!context.ok()) {                                                             \
                    return context.status();                                                     \
                }                                                                                \
                return static_cast<PluginType*>(instance)->bind(std::move(context.value()));     \
            },                                                                                   \
            out_status);                                                                         \
    }                                                                                            \
    rodaq_status_code rodaq_sdk_acquisition_configure(                                           \
        void* instance, rodaq_config_view config, rodaq_status* out_status) noexcept {           \
        if (instance == nullptr) {                                                               \
            return rodaq::plugin::invalid_input("instance is null", out_status);                 \
        }                                                                                        \
        return rodaq::plugin::invoke_void(                                                       \
            [instance, config]() -> rodaq::Result<void> {                                        \
                auto view = rodaq::plugin::ConfigView::from_abi(config);                         \
                if (!view.ok()) {                                                                \
                    return view.status();                                                        \
                }                                                                                \
                return static_cast<PluginType*>(instance)->configure(view.value());              \
            },                                                                                   \
            out_status);                                                                         \
    }                                                                                            \
    rodaq_status_code rodaq_sdk_acquisition_start(                                               \
        void* instance, rodaq_status* out_status) noexcept {                                     \
        if (instance == nullptr) {                                                               \
            rodaq::assign_abi_status(                                                            \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "instance is null", out_status);        \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                \
        }                                                                                        \
        return rodaq::plugin::invoke_void(                                                       \
            [instance] { return static_cast<PluginType*>(instance)->start(); }, out_status);     \
    }                                                                                            \
    rodaq_status_code rodaq_sdk_acquisition_stop(                                                \
        void* instance, rodaq_status* out_status) noexcept {                                     \
        if (instance == nullptr) {                                                               \
            rodaq::assign_abi_status(                                                            \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "instance is null", out_status);        \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                \
        }                                                                                        \
        return rodaq::plugin::invoke_void(                                                       \
            [instance] { return static_cast<PluginType*>(instance)->stop(); }, out_status);      \
    }                                                                                            \
    rodaq_status_code rodaq_sdk_acquisition_prepare(                                             \
        void* context, rodaq_binding_transition transition, rodaq_stream_handle stream,          \
        rodaq_stream_descriptor_handle old_descriptor,                                           \
        rodaq_stream_descriptor_handle new_descriptor, rodaq_status* out_status) noexcept {      \
        if (context == nullptr || transition.value == 0 || stream.value == 0 ||                  \
            new_descriptor.value == 0) {                                                         \
            return rodaq::plugin::invalid_input("binding prepare input is invalid", out_status); \
        }                                                                                        \
        return rodaq::plugin::invoke_void(                                                       \
            [context, transition, stream, old_descriptor, new_descriptor] {                      \
                return static_cast<PluginType*>(context)->prepare_binding_switch(                \
                    transition, stream, old_descriptor, new_descriptor);                         \
            },                                                                                   \
            out_status);                                                                         \
    }                                                                                            \
    rodaq_status_code rodaq_sdk_acquisition_commit(                                              \
        void* context, rodaq_binding_transition transition, rodaq_status* out_status) noexcept { \
        if (context == nullptr || transition.value == 0) {                                       \
            return rodaq::plugin::invalid_input("binding commit input is invalid", out_status);  \
        }                                                                                        \
        return rodaq::plugin::invoke_void(                                                       \
            [context, transition] {                                                              \
                return static_cast<PluginType*>(context)->commit_binding_switch(transition);     \
            },                                                                                   \
            out_status);                                                                         \
    }                                                                                            \
    rodaq_status_code rodaq_sdk_acquisition_abort(                                               \
        void* context, rodaq_binding_transition transition,                                      \
        rodaq_binding_abort_disposition disposition, rodaq_status* out_status) noexcept {        \
        if (context == nullptr || transition.value == 0 ||                                       \
            (disposition != RODAQ_BINDING_ABORT_KEEP_PAUSED &&                                   \
             disposition != RODAQ_BINDING_ABORT_RESUME_OLD)) {                                   \
            return rodaq::plugin::invalid_input("binding abort input is invalid", out_status);   \
        }                                                                                        \
        return rodaq::plugin::invoke_void(                                                       \
            [context, transition, disposition] {                                                 \
                return static_cast<PluginType*>(context)->abort_binding_switch(                  \
                    transition, disposition);                                                    \
            },                                                                                   \
            out_status);                                                                         \
    }                                                                                            \
    rodaq_status_code rodaq_sdk_acquisition_quiesce_stream(                                      \
        void* context, rodaq_stream_handle stream, rodaq_status* out_status) noexcept {          \
        if (context == nullptr || stream.value == 0) {                                           \
            return rodaq::plugin::invalid_input("stream quiesce input is invalid", out_status);  \
        }                                                                                        \
        return rodaq::plugin::invoke_void(                                                       \
            [context, stream] {                                                                  \
                return static_cast<PluginType*>(context)->quiesce_stream(stream);                \
            },                                                                                   \
            out_status);                                                                         \
    }                                                                                            \
    rodaq_status_code rodaq_sdk_acquisition_quiesce_plugin(                                      \
        void* context, rodaq_status* out_status) noexcept {                                      \
        if (context == nullptr) {                                                                \
            return rodaq::plugin::invalid_input("instance is null", out_status);                 \
        }                                                                                        \
        return rodaq::plugin::invoke_void(                                                       \
            [context] { return static_cast<PluginType*>(context)->quiesce_plugin(); },           \
            out_status);                                                                         \
    }                                                                                            \
    const rodaq_acquisition_control rodaq_sdk_acquisition_control = {                            \
        RODAQ_ACQUISITION_CONTROL_PROTOCOL_VERSION,                                              \
        RODAQ_ACQUISITION_CONTROL_MIN_STRUCT_SIZE,                                               \
        &rodaq_sdk_acquisition_prepare,                                                          \
        &rodaq_sdk_acquisition_commit,                                                           \
        &rodaq_sdk_acquisition_abort,                                                            \
        &rodaq_sdk_acquisition_quiesce_stream,                                                   \
        &rodaq_sdk_acquisition_quiesce_plugin,                                                   \
    };                                                                                           \
    rodaq_status_code rodaq_sdk_acquisition_query(                                               \
        void* instance, rodaq_string_view interface_id, rodaq_interface* out_interface,          \
        rodaq_status* out_status) noexcept {                                                     \
        if (instance == nullptr || out_interface == nullptr) {                                   \
            return rodaq::plugin::invalid_input("interface query input is invalid", out_status); \
        }                                                                                        \
        std::string_view parsed_id;                                                              \
        if (!rodaq::parse_abi_string(interface_id, &parsed_id)) {                                \
            return rodaq::plugin::invalid_input("interface id is invalid", out_status);          \
        }                                                                                        \
        const char required[] = RODAQ_ACQUISITION_CONTROL_INTERFACE_ID;                          \
        if (parsed_id == required) {                                                             \
            return rodaq::plugin::invoke_interface(                                              \
                [instance] {                                                                     \
                    return rodaq::Result<rodaq::plugin::InterfaceView>(                          \
                        rodaq::plugin::InterfaceView(                                            \
                            rodaq_interface{instance, &rodaq_sdk_acquisition_control}));         \
                },                                                                               \
                out_interface, out_status);                                                      \
        }                                                                                        \
        return rodaq::plugin::invoke_interface(                                                  \
            [instance, parsed_id] {                                                              \
                return static_cast<PluginType*>(instance)->query_interface(parsed_id);           \
            },                                                                                   \
            out_interface, out_status);                                                          \
    }                                                                                            \
    const rodaq_acquisition_plugin rodaq_sdk_acquisition_plugin = {                              \
        RODAQ_ACQUISITION_PROTOCOL_VERSION,                                                      \
        RODAQ_ACQUISITION_PLUGIN_MIN_STRUCT_SIZE,                                                \
        &(Descriptor),                                                                           \
        &rodaq_sdk_acquisition_create,                                                           \
        &rodaq_sdk_acquisition_destroy,                                                          \
        &rodaq_sdk_acquisition_bind,                                                             \
        &rodaq_sdk_acquisition_configure,                                                        \
        &rodaq_sdk_acquisition_start,                                                            \
        &rodaq_sdk_acquisition_stop,                                                             \
        &rodaq_sdk_acquisition_query,                                                            \
    };                                                                                           \
    }                                                                                            \
    extern "C" RODAQ_PLUGIN_EXPORT const std::uint32_t rodaq_plugin_abi_version =                \
        RODAQ_PLUGIN_ABI_VERSION;                                                                \
    extern "C" RODAQ_PLUGIN_EXPORT const rodaq_acquisition_plugin* rodaq_get_acquisition_plugin( \
        void) noexcept {                                                                         \
        return &rodaq_sdk_acquisition_plugin;                                                    \
    }

#endif  // RODAQ_PLUGIN_ACQUISITION_PLUGIN_HPP_
