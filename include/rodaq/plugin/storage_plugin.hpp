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
    virtual Result<void> bind(StorageRuntime runtime) = 0;
    virtual Result<void> configure(const ConfigView& config) = 0;
    virtual Result<void> open(const rodaq_storage_open_request& request) = 0;
    virtual Result<rodaq_storage_capabilities> get_capabilities() = 0;
    virtual Result<rodaq_descriptor_readiness_result> ensure_descriptor_ready(
        rodaq_stream_descriptor_handle descriptor) = 0;
    virtual Result<rodaq_storage_submit_result> submit(
        const rodaq_storage_submission& submission) = 0;
    virtual Result<void> flush() = 0;
    virtual Result<void> close(rodaq_storage_close_disposition disposition) = 0;
    virtual Result<InterfaceView> query_interface(std::string_view interface_id);
};

}  // namespace rodaq

namespace rodaq::plugin {

bool is_valid_storage_open_request(const rodaq_storage_open_request& request) noexcept;

bool is_valid_storage_submission(const rodaq_storage_submission& submission) noexcept;

bool is_valid_storage_submit_result(const rodaq_storage_submit_result& result) noexcept;

void write_storage_capabilities(
    rodaq_storage_capabilities* output, std::uint32_t capacity,
    const rodaq_storage_capabilities& value) noexcept;

void write_storage_submit_result(
    rodaq_storage_submit_result* output, std::uint32_t capacity,
    const rodaq_storage_submit_result& value) noexcept;

void write_descriptor_readiness_result(
    rodaq_descriptor_readiness_result* output, std::uint32_t capacity,
    const rodaq_descriptor_readiness_result& value) noexcept;

}  // namespace rodaq::plugin

#define RODAQ_EXPORT_STORAGE_PLUGIN(PluginType, Descriptor)                                        \
    namespace {                                                                                    \
    rodaq_status_code rodaq_sdk_storage_create(                                                    \
        void** out_instance, rodaq_status* out_status) noexcept {                                  \
        if (out_instance == nullptr) {                                                             \
            rodaq::assign_abi_status(                                                              \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "create output is null", out_status);     \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
        *out_instance = nullptr;                                                                   \
        try {                                                                                      \
            *out_instance = new PluginType();                                                      \
            return RODAQ_STATUS_OK;                                                                \
        } catch (const std::bad_alloc&) {                                                          \
            rodaq::assign_abi_status(                                                              \
                RODAQ_STATUS_RESOURCE_EXHAUSTED, "sdk", 0, "Plugin allocation failed",             \
                out_status);                                                                       \
            return RODAQ_STATUS_RESOURCE_EXHAUSTED;                                                \
        } catch (const std::exception& error) {                                                    \
            rodaq::assign_abi_status(                                                              \
                RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, error.what(), out_status);                  \
            return RODAQ_STATUS_INTERNAL_ERROR;                                                    \
        } catch (...) {                                                                            \
            rodaq::assign_abi_status(                                                              \
                RODAQ_STATUS_INTERNAL_ERROR, "sdk", 0, "Unknown create exception", out_status);    \
            return RODAQ_STATUS_INTERNAL_ERROR;                                                    \
        }                                                                                          \
    }                                                                                              \
    void rodaq_sdk_storage_destroy(void* instance) noexcept {                                      \
        delete static_cast<PluginType*>(instance);                                                 \
    }                                                                                              \
    rodaq_status_code rodaq_sdk_storage_bind(                                                      \
        void* instance, const rodaq_service_registry* services,                                    \
        rodaq_status* out_status) noexcept {                                                       \
        if (instance == nullptr || services == nullptr) {                                          \
            rodaq::assign_abi_status(                                                              \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "bind input is null", out_status);        \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
        return rodaq::plugin::invoke_void(                                                         \
            [instance, services]() -> rodaq::Result<void> {                                        \
                auto registry = rodaq::ServiceRegistryView::from_abi(*services);                   \
                if (!registry.ok()) {                                                              \
                    return registry.status();                                                      \
                }                                                                                  \
                auto runtime = rodaq::StorageRuntime::bind(registry.value());                      \
                if (!runtime.ok()) {                                                               \
                    return runtime.status();                                                       \
                }                                                                                  \
                return static_cast<PluginType*>(instance)->bind(std::move(runtime.value()));       \
            },                                                                                     \
            out_status);                                                                           \
    }                                                                                              \
    rodaq_status_code rodaq_sdk_storage_configure(                                                 \
        void* instance, rodaq_config_view config, rodaq_status* out_status) noexcept {             \
        if (instance == nullptr) {                                                                 \
            return rodaq::plugin::invalid_input("instance is null", out_status);                   \
        }                                                                                          \
        return rodaq::plugin::invoke_void(                                                         \
            [instance, config]() -> rodaq::Result<void> {                                          \
                auto view = rodaq::ConfigView::from_abi(config);                                   \
                if (!view.ok()) {                                                                  \
                    return view.status();                                                          \
                }                                                                                  \
                return static_cast<PluginType*>(instance)->configure(view.value());                \
            },                                                                                     \
            out_status);                                                                           \
    }                                                                                              \
    rodaq_status_code rodaq_sdk_storage_open(                                                      \
        void* instance, const rodaq_storage_open_request* request,                                 \
        rodaq_status* out_status) noexcept {                                                       \
        if (instance == nullptr || request == nullptr ||                                           \
            !rodaq::plugin::is_valid_storage_open_request(*request)) {                             \
            rodaq::assign_abi_status(                                                              \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "open input is invalid", out_status);     \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
        return rodaq::plugin::invoke_void(                                                         \
            [instance, request] { return static_cast<PluginType*>(instance)->open(*request); },    \
            out_status);                                                                           \
    }                                                                                              \
    rodaq_status_code rodaq_sdk_storage_submit(                                                    \
        void* instance, const rodaq_storage_submission* submission,                                \
        rodaq_storage_submit_result* out_result, rodaq_status* out_status) noexcept {              \
        if (instance == nullptr || submission == nullptr || out_result == nullptr ||               \
            !rodaq::plugin::is_valid_storage_submission(*submission)) {                            \
            rodaq::assign_abi_status(                                                              \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "submit input is invalid", out_status);   \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
        return rodaq::plugin::invoke_struct_output(                                                \
            out_result, RODAQ_STORAGE_SUBMIT_RESULT_MIN_STRUCT_SIZE,                               \
            [instance, submission] {                                                               \
                return static_cast<PluginType*>(instance)->submit(*submission);                    \
            },                                                                                     \
            [](auto* output, std::uint32_t capacity, const auto& value) {                          \
                rodaq::plugin::write_storage_submit_result(output, capacity, value);               \
            },                                                                                     \
            out_status);                                                                           \
    }                                                                                              \
    rodaq_status_code rodaq_sdk_storage_flush(void* instance, rodaq_status* out_status) noexcept { \
        if (instance == nullptr) {                                                                 \
            return rodaq::plugin::invalid_input("instance is null", out_status);                   \
        }                                                                                          \
        return rodaq::plugin::invoke_void(                                                         \
            [instance] { return static_cast<PluginType*>(instance)->flush(); }, out_status);       \
    }                                                                                              \
    rodaq_status_code rodaq_sdk_storage_close(                                                     \
        void* instance, const rodaq_storage_close_request* request,                                \
        rodaq_status* out_status) noexcept {                                                       \
        if (instance == nullptr || request == nullptr ||                                           \
            request->struct_size < RODAQ_STORAGE_CLOSE_REQUEST_MIN_STRUCT_SIZE ||                  \
            request->reserved[0] != 0 || request->reserved[1] != 0 ||                              \
            (request->disposition != RODAQ_STORAGE_CLOSE_NORMAL &&                                 \
             request->disposition != RODAQ_STORAGE_CLOSE_ACTIVATION_ABORT &&                       \
             request->disposition != RODAQ_STORAGE_CLOSE_FAILURE)) {                               \
            rodaq::assign_abi_status(                                                              \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "close input is invalid", out_status);    \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
        return rodaq::plugin::invoke_void(                                                         \
            [instance, request] {                                                                  \
                return static_cast<PluginType*>(instance)->close(request->disposition);            \
            },                                                                                     \
            out_status);                                                                           \
    }                                                                                              \
    rodaq_status_code rodaq_sdk_storage_capabilities(                                              \
        void* context, rodaq_storage_capabilities* out_capabilities,                               \
        rodaq_status* out_status) noexcept {                                                       \
        if (context == nullptr) {                                                                  \
            return rodaq::plugin::invalid_input("instance is null", out_status);                   \
        }                                                                                          \
        return rodaq::plugin::invoke_struct_output(                                                \
            out_capabilities, RODAQ_STORAGE_CAPABILITIES_MIN_STRUCT_SIZE,                          \
            [context] { return static_cast<PluginType*>(context)->get_capabilities(); },           \
            [](auto* output, std::uint32_t capacity, const auto& value) {                          \
                rodaq::plugin::write_storage_capabilities(output, capacity, value);                \
            },                                                                                     \
            out_status);                                                                           \
    }                                                                                              \
    rodaq_status_code rodaq_sdk_storage_ready(                                                     \
        void* context, rodaq_stream_descriptor_handle descriptor,                                  \
        rodaq_descriptor_readiness_result* out_result, rodaq_status* out_status) noexcept {        \
        if (context == nullptr || descriptor.value == 0) {                                         \
            return rodaq::plugin::invalid_input(                                                   \
                "descriptor readiness input is invalid", out_status);                              \
        }                                                                                          \
        return rodaq::plugin::invoke_struct_output(                                                \
            out_result, RODAQ_DESCRIPTOR_READINESS_RESULT_MIN_STRUCT_SIZE,                         \
            [context, descriptor] {                                                                \
                return static_cast<PluginType*>(context)->ensure_descriptor_ready(descriptor);     \
            },                                                                                     \
            [](auto* output, std::uint32_t capacity, const auto& value) {                          \
                rodaq::plugin::write_descriptor_readiness_result(output, capacity, value);         \
            },                                                                                     \
            out_status);                                                                           \
    }                                                                                              \
    const rodaq_storage_control_v1 rodaq_sdk_storage_control = {                                   \
        RODAQ_STORAGE_CONTROL_PROTOCOL_VERSION,                                                    \
        RODAQ_STORAGE_CONTROL_V1_MIN_STRUCT_SIZE,                                                  \
        &rodaq_sdk_storage_capabilities,                                                           \
        &rodaq_sdk_storage_ready,                                                                  \
    };                                                                                             \
    rodaq_status_code rodaq_sdk_storage_query(                                                     \
        void* instance, rodaq_string_view interface_id, rodaq_interface* out_interface,            \
        rodaq_status* out_status) noexcept {                                                       \
        if (instance == nullptr || out_interface == nullptr) {                                     \
            return rodaq::plugin::invalid_input("interface query input is invalid", out_status);   \
        }                                                                                          \
        std::string_view parsed_id;                                                                \
        if (!rodaq::parse_abi_string(interface_id, &parsed_id)) {                                  \
            return rodaq::plugin::invalid_input("interface id is invalid", out_status);            \
        }                                                                                          \
        const char required[] = RODAQ_STORAGE_CONTROL_INTERFACE_ID;                                \
        if (parsed_id == required) {                                                               \
            return rodaq::plugin::invoke_interface(                                                \
                [instance] {                                                                       \
                    return rodaq::Result<rodaq::InterfaceView>(rodaq::InterfaceView(               \
                        rodaq_interface{instance, &rodaq_sdk_storage_control}));                   \
                },                                                                                 \
                out_interface, out_status);                                                        \
        }                                                                                          \
        return rodaq::plugin::invoke_interface(                                                    \
            [instance, parsed_id] {                                                                \
                return static_cast<PluginType*>(instance)->query_interface(parsed_id);             \
            },                                                                                     \
            out_interface, out_status);                                                            \
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
