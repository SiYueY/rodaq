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

namespace rodaq::plugin {

inline bool is_valid_storage_open_request(const rodaq_storage_open_request& request) noexcept {
    if (request.struct_size < RODAQ_STORAGE_OPEN_REQUEST_MIN_STRUCT_SIZE || request.flags != 0) {
        return false;
    }
    std::string_view session_id;
    std::string_view target_uri;
    return parse_abi_string(request.session_id, &session_id) &&
           parse_abi_string(request.target_uri, &target_uri);
}

inline bool is_valid_storage_submission(const rodaq_storage_submission& submission) noexcept {
    if (submission.struct_size < RODAQ_STORAGE_SUBMISSION_MIN_STRUCT_SIZE ||
        submission.flags != 0 || submission.reserved != 0 || submission.operation.value == 0 ||
        submission.stream.value == 0 || submission.descriptor.value == 0 ||
        !rodaq::is_valid_abi_bool(submission.source_present) ||
        !rodaq::is_valid_abi_bool(submission.source_sequence_present)) {
        return false;
    }
    if (!submission.source_present && submission.source_sequence_present) {
        return false;
    }
    if (submission.source_present && submission.source.value == 0) {
        return false;
    }
    if (!rodaq::is_valid_timestamp_values(
            submission.timestamp_values, submission.timestamp_value_count)) {
        return false;
    }
    const std::uint8_t* payload_data = nullptr;
    std::size_t payload_size = 0;
    return parse_abi_bytes(submission.payload, &payload_data, &payload_size);
}

inline void write_storage_capabilities(
    rodaq_storage_capabilities* output, std::uint32_t capacity,
    const rodaq_storage_capabilities& value) noexcept {
    clear_output_prefix(output, capacity);
    output->struct_size = sizeof(rodaq_storage_capabilities);
    output->flags = value.flags;
    output->full_fidelity = value.full_fidelity;
    output->reserved = 0;
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_storage_capabilities, capacity, maximum_payload_bytes)) {
        output->maximum_payload_bytes = value.maximum_payload_bytes;
    }
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_storage_capabilities, capacity, maximum_inflight_records)) {
        output->maximum_inflight_records = value.maximum_inflight_records;
    }
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_storage_capabilities, capacity, maximum_inflight_bytes)) {
        output->maximum_inflight_bytes = value.maximum_inflight_bytes;
    }
    if (RODAQ_HAS_FIELD_CAPACITY(
            rodaq_storage_capabilities, capacity, handoff_progress_timeout_ns)) {
        output->handoff_progress_timeout_ns = value.handoff_progress_timeout_ns;
    }
    if (RODAQ_HAS_FIELD_CAPACITY(
            rodaq_storage_capabilities, capacity, descriptor_progress_timeout_ns)) {
        output->descriptor_progress_timeout_ns = value.descriptor_progress_timeout_ns;
    }
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_storage_capabilities, capacity, flush_progress_timeout_ns)) {
        output->flush_progress_timeout_ns = value.flush_progress_timeout_ns;
    }
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_storage_capabilities, capacity, close_progress_timeout_ns)) {
        output->close_progress_timeout_ns = value.close_progress_timeout_ns;
    }
}

inline void write_storage_submit_result(
    rodaq_storage_submit_result* output, std::uint32_t capacity,
    const rodaq_storage_submit_result& value) noexcept {
    clear_output_prefix(output, capacity);
    output->struct_size = sizeof(rodaq_storage_submit_result);
    output->reserved = 0;
    output->payload_state = value.payload_state;
    output->persistence_state = value.persistence_state;
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_storage_submit_result, capacity, persistence_error)) {
        output->persistence_error = value.persistence_error;
    }
}

inline void write_descriptor_readiness_result(
    rodaq_descriptor_readiness_result* output, std::uint32_t capacity,
    const rodaq_descriptor_readiness_result& value) noexcept {
    clear_output_prefix(output, capacity);
    output->struct_size = sizeof(rodaq_descriptor_readiness_result);
    output->state = value.state;
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_descriptor_readiness_result, capacity, failure)) {
        output->failure = value.failure;
    }
}

}  // namespace rodaq::plugin

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
        if (instance == nullptr || services == nullptr) {                                          \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "bind input is null", out_error);         \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
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
        if (instance == nullptr) {                                                                 \
            return rodaq::plugin::invalid_input("instance is null", out_error);                    \
        }                                                                                          \
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
        if (instance == nullptr || request == nullptr ||                                           \
            !rodaq::plugin::is_valid_storage_open_request(*request)) {                             \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "open input is invalid", out_error);      \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
        return rodaq::plugin::invoke_void(                                                         \
            [instance, request] { return static_cast<PluginType*>(instance)->Open(*request); },    \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_storage_submit(                                                         \
        void* instance, const rodaq_storage_submission* submission,                                \
        rodaq_storage_submit_result* out_result, rodaq_error* out_error) noexcept {                \
        if (instance == nullptr || submission == nullptr || out_result == nullptr ||               \
            !rodaq::plugin::is_valid_storage_submission(*submission)) {                            \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "submit input is invalid", out_error);    \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
        return rodaq::plugin::invoke_struct_output(                                                \
            out_result, RODAQ_STORAGE_SUBMIT_RESULT_MIN_STRUCT_SIZE,                               \
            [instance, submission] {                                                               \
                return static_cast<PluginType*>(instance)->Submit(*submission);                    \
            },                                                                                     \
            [](auto* output, std::uint32_t capacity, const auto& value) {                          \
                rodaq::plugin::write_storage_submit_result(output, capacity, value);               \
            },                                                                                     \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_storage_flush(void* instance, rodaq_error* out_error) noexcept {        \
        if (instance == nullptr) {                                                                 \
            return rodaq::plugin::invalid_input("instance is null", out_error);                    \
        }                                                                                          \
        return rodaq::plugin::invoke_void(                                                         \
            [instance] { return static_cast<PluginType*>(instance)->Flush(); }, out_error);        \
    }                                                                                              \
    rodaq_status rodaq_sdk_storage_close(                                                          \
        void* instance, const rodaq_storage_close_request* request,                                \
        rodaq_error* out_error) noexcept {                                                         \
        if (instance == nullptr || request == nullptr ||                                           \
            request->struct_size < RODAQ_STORAGE_CLOSE_REQUEST_MIN_STRUCT_SIZE ||                  \
            request->reserved[0] != 0 || request->reserved[1] != 0 ||                              \
            (request->disposition != RODAQ_STORAGE_CLOSE_NORMAL &&                                 \
             request->disposition != RODAQ_STORAGE_CLOSE_ACTIVATION_ABORT &&                       \
             request->disposition != RODAQ_STORAGE_CLOSE_FAILURE)) {                               \
            rodaq::assign_rodaq_error(                                                             \
                RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, "close input is invalid", out_error);     \
            return RODAQ_STATUS_INVALID_ARGUMENT;                                                  \
        }                                                                                          \
        return rodaq::plugin::invoke_void(                                                         \
            [instance, request] {                                                                  \
                return static_cast<PluginType*>(instance)->Close(request->disposition);            \
            },                                                                                     \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_storage_capabilities(                                                   \
        void* context, rodaq_storage_capabilities* out_capabilities,                               \
        rodaq_error* out_error) noexcept {                                                         \
        if (context == nullptr) {                                                                  \
            return rodaq::plugin::invalid_input("instance is null", out_error);                    \
        }                                                                                          \
        return rodaq::plugin::invoke_struct_output(                                                \
            out_capabilities, RODAQ_STORAGE_CAPABILITIES_MIN_STRUCT_SIZE,                          \
            [context] { return static_cast<PluginType*>(context)->GetCapabilities(); },            \
            [](auto* output, std::uint32_t capacity, const auto& value) {                          \
                rodaq::plugin::write_storage_capabilities(output, capacity, value);                \
            },                                                                                     \
            out_error);                                                                            \
    }                                                                                              \
    rodaq_status rodaq_sdk_storage_ready(                                                          \
        void* context, rodaq_stream_descriptor_handle descriptor,                                  \
        rodaq_descriptor_readiness_result* out_result, rodaq_error* out_error) noexcept {          \
        if (context == nullptr || descriptor.value == 0) {                                         \
            return rodaq::plugin::invalid_input(                                                   \
                "descriptor readiness input is invalid", out_error);                               \
        }                                                                                          \
        return rodaq::plugin::invoke_struct_output(                                                \
            out_result, RODAQ_DESCRIPTOR_READINESS_RESULT_MIN_STRUCT_SIZE,                         \
            [context, descriptor] {                                                                \
                return static_cast<PluginType*>(context)->EnsureDescriptorReady(descriptor);       \
            },                                                                                     \
            [](auto* output, std::uint32_t capacity, const auto& value) {                          \
                rodaq::plugin::write_descriptor_readiness_result(output, capacity, value);         \
            },                                                                                     \
            out_error);                                                                            \
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
        if (instance == nullptr || out_interface == nullptr) {                                     \
            return rodaq::plugin::invalid_input("interface query input is invalid", out_error);    \
        }                                                                                          \
        std::string_view parsed_id;                                                                \
        if (!rodaq::parse_abi_string(interface_id, &parsed_id)) {                                  \
            return rodaq::plugin::invalid_input("interface id is invalid", out_error);             \
        }                                                                                          \
        const char required[] = RODAQ_STORAGE_CONTROL_INTERFACE_ID;                                \
        if (parsed_id == required) {                                                               \
            return rodaq::plugin::invoke_interface(                                                \
                [instance] {                                                                       \
                    return rodaq::Result<rodaq::InterfaceView>(rodaq::InterfaceView(               \
                        rodaq_interface{instance, &rodaq_sdk_storage_control}));                   \
                },                                                                                 \
                out_interface, out_error);                                                         \
        }                                                                                          \
        return rodaq::plugin::invoke_interface(                                                    \
            [instance, parsed_id] {                                                                \
                return static_cast<PluginType*>(instance)->QueryInterface(parsed_id);              \
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
