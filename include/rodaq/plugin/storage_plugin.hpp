#ifndef RODAQ_PLUGIN_STORAGE_PLUGIN_HPP_
#define RODAQ_PLUGIN_STORAGE_PLUGIN_HPP_

#include <rodaq/plugin/abi/storage.h>
#include <rodaq/plugin/config_view.hpp>
#include <rodaq/plugin/plugin.hpp>
#include <rodaq/plugin/runtime_services.hpp>

#include <string_view>

namespace rodaq {

class StoragePlugin {
public:
    virtual ~StoragePlugin() = default;
    virtual Result<void> Bind(StorageRuntime runtime) = 0;
    virtual Result<void> Configure(const ConfigView& config) = 0;
    virtual Result<void> Open(const rodaq_storage_open_request& request) = 0;
    virtual Result<rodaq_storage_capabilities> GetCapabilities() = 0;
    virtual Result<rodaq_descriptor_readiness_result> EnsureDescriptorReady(
        rodaq_stream_descriptor_handle descriptor) = 0;
    virtual Result<rodaq_storage_submit_result> Submit(
        const rodaq_storage_submission& submission) = 0;
    virtual Result<void> Flush() = 0;
    virtual Result<void> Close(rodaq_storage_close_disposition disposition) = 0;
    virtual Result<InterfaceView> QueryInterface(std::string_view interface_id) {
        static_cast<void>(interface_id);
        return nonstd::make_unexpected(
            Error(ErrorCode::NotFound, "sdk", 0, "Optional Plugin interface is unavailable"));
    }
};

}  // namespace rodaq

#define RODAQ_EXPORT_STORAGE_PLUGIN(PluginType, Descriptor)                                        \
    namespace {                                                                                    \
    rodaq_status rodaq_sdk_storage_create(void** out_instance, rodaq_error* out_error) noexcept {  \
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
    void rodaq_sdk_storage_destroy(void* instance) noexcept {                                      \
        delete static_cast<PluginType*>(instance);                                                 \
    }                                                                                              \
    rodaq_status rodaq_sdk_storage_bind(                                                           \
        void* instance, const rodaq_service_registry* services, rodaq_error* out_error) noexcept { \
        return rodaq::plugin::invoke_void(                                                         \
            [instance, services] {                                                                 \
                auto registry = rodaq::ServiceRegistryView::FromAbi(*services);                    \
                if (!registry.has_value()) {                                                       \
                    return rodaq::Result<void>(                                                    \
                        nonstd::make_unexpected(std::move(registry.error())));                     \
                }                                                                                  \
                auto runtime = rodaq::StorageRuntime::Bind(registry.value());                      \
                if (!runtime.has_value()) {                                                        \
                    return rodaq::Result<void>(                                                    \
                        nonstd::make_unexpected(std::move(runtime.error())));                      \
                }                                                                                  \
                return static_cast<PluginType*>(instance)->Bind(std::move(runtime.value()));       \
            },                                                                                     \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_storage_configure(                                                      \
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
    rodaq_status rodaq_sdk_storage_open(                                                           \
        void* instance, const rodaq_storage_open_request* request,                                 \
        rodaq_error* out_error) noexcept {                                                         \
        return rodaq::plugin::invoke_void(                                                         \
            [instance, request] { return static_cast<PluginType*>(instance)->Open(*request); },    \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_storage_submit(                                                         \
        void* instance, const rodaq_storage_submission* submission,                                \
        rodaq_storage_submit_result* out_result, rodaq_error* out_error) noexcept {                \
        if (out_result == nullptr) {                                                               \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "submit output is null", out_error);      \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
        *out_result = {};                                                                          \
        try {                                                                                      \
            auto result = static_cast<PluginType*>(instance)->Submit(*submission);                 \
            if (!result.has_value()) {                                                             \
                rodaq::assign_rodaq_error(result.error(), out_error);                              \
                return rodaq::status_from_error_code(result.error().code());                       \
            }                                                                                      \
            *out_result = result.value();                                                          \
            return RODAQ_STATUS_OK;                                                                \
        } catch (const std::exception& error) {                                                    \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, error.what(), out_error);                   \
            return RODAQ_STATUS_INTERNAL_ERROR;                                                    \
        } catch (...) {                                                                            \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, "Unknown submit exception", out_error);     \
            return RODAQ_STATUS_INTERNAL_ERROR;                                                    \
        }                                                                                          \
    }                                                                                              \
    rodaq_status rodaq_sdk_storage_flush(void* instance, rodaq_error* out_error) noexcept {        \
        return rodaq::plugin::invoke_void(                                                         \
            [instance] { return static_cast<PluginType*>(instance)->Flush(); }, out_error);        \
    }                                                                                              \
    rodaq_status rodaq_sdk_storage_close(                                                          \
        void* instance, const rodaq_storage_close_request* request,                                \
        rodaq_error* out_error) noexcept {                                                         \
        return rodaq::plugin::invoke_void(                                                         \
            [instance, request] {                                                                  \
                return static_cast<PluginType*>(instance)->Close(request->disposition);            \
            },                                                                                     \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_storage_capabilities(                                                   \
        void* context, rodaq_storage_capabilities* out_capabilities,                               \
        rodaq_error* out_error) noexcept {                                                         \
        if (out_capabilities == nullptr) {                                                         \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "capabilities output is null",            \
                out_error);                                                                        \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
        *out_capabilities = {};                                                                    \
        try {                                                                                      \
            auto result = static_cast<PluginType*>(context)->GetCapabilities();                    \
            if (!result.has_value()) {                                                             \
                rodaq::assign_rodaq_error(result.error(), out_error);                              \
                return rodaq::status_from_error_code(result.error().code());                       \
            }                                                                                      \
            *out_capabilities = result.value();                                                    \
            return RODAQ_STATUS_OK;                                                                \
        } catch (const std::exception& error) {                                                    \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, error.what(), out_error);                   \
            return RODAQ_STATUS_INTERNAL_ERROR;                                                    \
        } catch (...) {                                                                            \
            return RODAQ_STATUS_INTERNAL_ERROR;                                                    \
        }                                                                                          \
    }                                                                                              \
    rodaq_status rodaq_sdk_storage_ready(                                                          \
        void* context, rodaq_stream_descriptor_handle descriptor,                                  \
        rodaq_descriptor_readiness_result* out_result, rodaq_error* out_error) noexcept {          \
        if (out_result == nullptr) {                                                               \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "readiness output is null", out_error);   \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
        *out_result = {};                                                                          \
        try {                                                                                      \
            auto result = static_cast<PluginType*>(context)->EnsureDescriptorReady(descriptor);    \
            if (!result.has_value()) {                                                             \
                rodaq::assign_rodaq_error(result.error(), out_error);                              \
                return rodaq::status_from_error_code(result.error().code());                       \
            }                                                                                      \
            *out_result = result.value();                                                          \
            return RODAQ_STATUS_OK;                                                                \
        } catch (const std::exception& error) {                                                    \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, error.what(), out_error);                   \
            return RODAQ_STATUS_INTERNAL_ERROR;                                                    \
        } catch (...) {                                                                            \
            return RODAQ_STATUS_INTERNAL_ERROR;                                                    \
        }                                                                                          \
    }                                                                                              \
    const rodaq_storage_control_v1 rodaq_sdk_storage_control = {                                   \
        RODAQ_STORAGE_CONTROL_PROTOCOL_VERSION,                                                    \
        RODAQ_STORAGE_CONTROL_V1_MIN_STRUCT_SIZE,                                                  \
        &rodaq_sdk_storage_capabilities,                                                           \
        &rodaq_sdk_storage_ready,                                                                  \
    };                                                                                             \
    rodaq_status rodaq_sdk_storage_query(                                                          \
        void* instance, rodaq_string_view interface_id, rodaq_interface* out_interface,            \
        rodaq_error* out_error) noexcept {                                                         \
        const char required[] = RODAQ_STORAGE_CONTROL_INTERFACE_ID;                                \
        if (interface_id.size == sizeof(required) - 1 &&                                           \
            std::string_view(interface_id.data, interface_id.size) == required) {                  \
            return rodaq::plugin::invoke_interface(                                                \
                [instance] {                                                                       \
                    return rodaq::Result<rodaq::InterfaceView>(rodaq::InterfaceView(               \
                        rodaq_interface{instance, &rodaq_sdk_storage_control}));                   \
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
    const rodaq_storage_plugin rodaq_sdk_storage_plugin_table = {                                  \
        RODAQ_STORAGE_PROTOCOL_VERSION,                                                            \
        RODAQ_STORAGE_PLUGIN_MIN_STRUCT_SIZE,                                                      \
        &(Descriptor),                                                                             \
        &rodaq_sdk_storage_create,                                                                 \
        &rodaq_sdk_storage_destroy,                                                                \
        &rodaq_sdk_storage_bind,                                                                   \
        &rodaq_sdk_storage_configure,                                                              \
        &rodaq_sdk_storage_open,                                                                   \
        &rodaq_sdk_storage_submit,                                                                 \
        &rodaq_sdk_storage_flush,                                                                  \
        &rodaq_sdk_storage_close,                                                                  \
        &rodaq_sdk_storage_query,                                                                  \
    };                                                                                             \
    }                                                                                              \
    extern "C" RODAQ_PLUGIN_EXPORT const std::uint32_t rodaq_plugin_abi_version =                  \
        RODAQ_PLUGIN_ABI_VERSION;                                                                  \
    extern "C" RODAQ_PLUGIN_EXPORT const rodaq_storage_plugin* rodaq_get_storage_plugin(           \
        void) noexcept {                                                                           \
        return &rodaq_sdk_storage_plugin_table;                                                    \
    }

#endif  // RODAQ_PLUGIN_STORAGE_PLUGIN_HPP_
