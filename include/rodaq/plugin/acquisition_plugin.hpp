#ifndef RODAQ_PLUGIN_ACQUISITION_PLUGIN_HPP_
#define RODAQ_PLUGIN_ACQUISITION_PLUGIN_HPP_

#include <rodaq/plugin/abi/acquisition.h>
#include <rodaq/plugin/config_view.hpp>
#include <rodaq/plugin/plugin.hpp>
#include <rodaq/plugin/runtime_services.hpp>

#include <string_view>

namespace rodaq {

class AcquisitionPlugin {
public:
    virtual ~AcquisitionPlugin() = default;
    virtual Result<void> Bind(AcquisitionRuntime runtime) = 0;
    virtual Result<void> Configure(const ConfigView& config) = 0;
    virtual Result<void> Start() = 0;
    virtual Result<void> Stop() = 0;
    virtual Result<void> PrepareBindingSwitch(
        rodaq_binding_transition transition, rodaq_stream_handle stream,
        rodaq_stream_descriptor_handle old_descriptor,
        rodaq_stream_descriptor_handle new_descriptor) = 0;
    virtual Result<void> CommitBindingSwitch(rodaq_binding_transition transition) = 0;
    virtual Result<void> AbortBindingSwitch(
        rodaq_binding_transition transition, rodaq_binding_abort_disposition disposition) = 0;
    virtual Result<void> QuiesceStream(rodaq_stream_handle stream) = 0;
    virtual Result<void> QuiescePlugin() = 0;
    virtual Result<InterfaceView> QueryInterface(std::string_view interface_id) {
        static_cast<void>(interface_id);
        return nonstd::make_unexpected(
            Error(ErrorCode::NotFound, "sdk", 0, "Optional Plugin interface is unavailable"));
    }
};

}  // namespace rodaq

#define RODAQ_EXPORT_ACQUISITION_PLUGIN(PluginType, Descriptor)                                    \
    namespace {                                                                                    \
    rodaq_status rodaq_sdk_acquisition_create(                                                     \
        void** out_instance, rodaq_error* out_error) noexcept {                                    \
        if (out_instance == nullptr) {                                                             \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "create output is null", out_error);      \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
        *out_instance = nullptr;                                                                   \
        try {                                                                                      \
            *out_instance = new PluginType();                                                      \
            return RODAQ_STATUS_OK;                                                                \
        } catch (const std::bad_alloc&) {                                                          \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_RESOURCE_EXHAUSTED, "sdk", 0, "Plugin allocation failed", out_error); \
            return RODAQ_STATUS_RESOURCE_EXHAUSTED;                                                \
        } catch (const std::exception& error) {                                                    \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, error.what(), out_error);                   \
            return RODAQ_STATUS_INTERNAL_ERROR;                                                    \
        } catch (...) {                                                                            \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, "Unknown create exception", out_error);     \
            return RODAQ_STATUS_INTERNAL_ERROR;                                                    \
        }                                                                                          \
    }                                                                                              \
    void rodaq_sdk_acquisition_destroy(void* instance) noexcept {                                  \
        delete static_cast<PluginType*>(instance);                                                 \
    }                                                                                              \
    rodaq_status rodaq_sdk_acquisition_bind(                                                       \
        void* instance, const rodaq_service_registry* services, rodaq_error* out_error) noexcept { \
        return rodaq::plugin::invoke_void(                                                         \
            [instance, services] {                                                                 \
                auto registry = rodaq::ServiceRegistryView::FromAbi(*services);                    \
                if (!registry.has_value()) {                                                       \
                    return rodaq::Result<void>(                                                    \
                        nonstd::make_unexpected(std::move(registry.error())));                     \
                }                                                                                  \
                auto runtime = rodaq::AcquisitionRuntime::Bind(registry.value());                  \
                if (!runtime.has_value()) {                                                        \
                    return rodaq::Result<void>(                                                    \
                        nonstd::make_unexpected(std::move(runtime.error())));                      \
                }                                                                                  \
                return static_cast<PluginType*>(instance)->Bind(std::move(runtime.value()));       \
            },                                                                                     \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_acquisition_configure(                                                  \
        void* instance, rodaq_config_view config, rodaq_error* out_error) noexcept {               \
        return rodaq::plugin::invoke_void(                                                         \
            [instance, config] {                                                                   \
                auto view = rodaq::ConfigView::FromAbi(config);                                    \
                if (!view.has_value()) {                                                           \
                    return rodaq::Result<void>(nonstd::make_unexpected(std::move(view.error())));  \
                }                                                                                  \
                return static_cast<PluginType*>(instance)->Configure(view.value());                \
            },                                                                                     \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_acquisition_start(void* instance, rodaq_error* out_error) noexcept {    \
        return rodaq::plugin::invoke_void(                                                         \
            [instance] { return static_cast<PluginType*>(instance)->Start(); }, out_error);        \
    }                                                                                              \
    rodaq_status rodaq_sdk_acquisition_stop(void* instance, rodaq_error* out_error) noexcept {     \
        return rodaq::plugin::invoke_void(                                                         \
            [instance] { return static_cast<PluginType*>(instance)->Stop(); }, out_error);         \
    }                                                                                              \
    rodaq_status rodaq_sdk_acquisition_prepare(                                                    \
        void* context, rodaq_binding_transition transition, rodaq_stream_handle stream,            \
        rodaq_stream_descriptor_handle old_descriptor,                                             \
        rodaq_stream_descriptor_handle new_descriptor, rodaq_error* out_error) noexcept {          \
        return rodaq::plugin::invoke_void(                                                         \
            [context, transition, stream, old_descriptor, new_descriptor] {                        \
                return static_cast<PluginType*>(context)->PrepareBindingSwitch(                    \
                    transition, stream, old_descriptor, new_descriptor);                           \
            },                                                                                     \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_acquisition_commit(                                                     \
        void* context, rodaq_binding_transition transition, rodaq_error* out_error) noexcept {     \
        return rodaq::plugin::invoke_void(                                                         \
            [context, transition] {                                                                \
                return static_cast<PluginType*>(context)->CommitBindingSwitch(transition);         \
            },                                                                                     \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_acquisition_abort(                                                      \
        void* context, rodaq_binding_transition transition,                                        \
        rodaq_binding_abort_disposition disposition, rodaq_error* out_error) noexcept {            \
        return rodaq::plugin::invoke_void(                                                         \
            [context, transition, disposition] {                                                   \
                return static_cast<PluginType*>(context)->AbortBindingSwitch(                      \
                    transition, disposition);                                                      \
            },                                                                                     \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_acquisition_quiesce_stream(                                             \
        void* context, rodaq_stream_handle stream, rodaq_error* out_error) noexcept {              \
        return rodaq::plugin::invoke_void(                                                         \
            [context, stream] {                                                                    \
                return static_cast<PluginType*>(context)->QuiesceStream(stream);                   \
            },                                                                                     \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_acquisition_quiesce_plugin(                                             \
        void* context, rodaq_error* out_error) noexcept {                                          \
        return rodaq::plugin::invoke_void(                                                         \
            [context] { return static_cast<PluginType*>(context)->QuiescePlugin(); }, out_error);  \
    }                                                                                              \
    const rodaq_acquisition_control_v1 rodaq_sdk_acquisition_control = {                           \
        RODAQ_ACQUISITION_CONTROL_PROTOCOL_VERSION,                                                \
        RODAQ_ACQUISITION_CONTROL_V1_MIN_STRUCT_SIZE,                                              \
        &rodaq_sdk_acquisition_prepare,                                                            \
        &rodaq_sdk_acquisition_commit,                                                             \
        &rodaq_sdk_acquisition_abort,                                                              \
        &rodaq_sdk_acquisition_quiesce_stream,                                                     \
        &rodaq_sdk_acquisition_quiesce_plugin,                                                     \
    };                                                                                             \
    rodaq_status rodaq_sdk_acquisition_query(                                                      \
        void* instance, rodaq_string_view interface_id, rodaq_interface* out_interface,            \
        rodaq_error* out_error) noexcept {                                                         \
        const char required[] = RODAQ_ACQUISITION_CONTROL_INTERFACE_ID;                            \
        if (interface_id.size == sizeof(required) - 1 &&                                           \
            std::string_view(interface_id.data, interface_id.size) == required) {                  \
            return rodaq::plugin::invoke_interface(                                                \
                [instance] {                                                                       \
                    return rodaq::Result<rodaq::InterfaceView>(rodaq::InterfaceView(               \
                        rodaq_interface{instance, &rodaq_sdk_acquisition_control}));               \
                },                                                                                 \
                out_interface, out_error);                                                         \
        }                                                                                          \
        return rodaq::plugin::invoke_interface(                                                    \
            [instance, interface_id] {                                                             \
                return static_cast<PluginType*>(instance)->QueryInterface(                         \
                    std::string_view(interface_id.data, interface_id.size));                       \
            },                                                                                     \
            out_interface, out_error);                                                             \
    }                                                                                              \
    const rodaq_acquisition_plugin rodaq_sdk_acquisition_plugin = {                                \
        RODAQ_ACQUISITION_PROTOCOL_VERSION,                                                        \
        RODAQ_ACQUISITION_PLUGIN_MIN_STRUCT_SIZE,                                                  \
        &(Descriptor),                                                                             \
        &rodaq_sdk_acquisition_create,                                                             \
        &rodaq_sdk_acquisition_destroy,                                                            \
        &rodaq_sdk_acquisition_bind,                                                               \
        &rodaq_sdk_acquisition_configure,                                                          \
        &rodaq_sdk_acquisition_start,                                                              \
        &rodaq_sdk_acquisition_stop,                                                               \
        &rodaq_sdk_acquisition_query,                                                              \
    };                                                                                             \
    }                                                                                              \
    extern "C" RODAQ_PLUGIN_EXPORT const std::uint32_t rodaq_plugin_abi_version =                  \
        RODAQ_PLUGIN_ABI_VERSION;                                                                  \
    extern "C" RODAQ_PLUGIN_EXPORT const rodaq_acquisition_plugin* rodaq_get_acquisition_plugin(   \
        void) noexcept {                                                                           \
        return &rodaq_sdk_acquisition_plugin;                                                      \
    }

#endif  // RODAQ_PLUGIN_ACQUISITION_PLUGIN_HPP_
