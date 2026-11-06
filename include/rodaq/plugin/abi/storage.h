#ifndef RODAQ_PLUGIN_ABI_STORAGE_H_
#define RODAQ_PLUGIN_ABI_STORAGE_H_

#include <rodaq/plugin/abi/canonical.h>
#include <rodaq/plugin/abi/config.h>
#include <rodaq/plugin/abi/plugin.h>
#include <rodaq/plugin/abi/service.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RODAQ_STORAGE_CONTROL_INTERFACE_ID "rodaq.storage.control.v1"

typedef uint32_t rodaq_storage_close_disposition;

#define RODAQ_STORAGE_CLOSE_NORMAL UINT32_C(1)
#define RODAQ_STORAGE_CLOSE_ACTIVATION_ABORT UINT32_C(2)
#define RODAQ_STORAGE_CLOSE_FAILURE UINT32_C(3)

typedef uint32_t rodaq_storage_payload_state;

#define RODAQ_STORAGE_PAYLOAD_PENDING UINT32_C(1)
#define RODAQ_STORAGE_PAYLOAD_CONSUMED UINT32_C(2)

typedef uint32_t rodaq_storage_persistence_state;

#define RODAQ_STORAGE_PERSISTENCE_PENDING UINT32_C(1)
#define RODAQ_STORAGE_PERSISTENCE_SUCCEEDED UINT32_C(2)
#define RODAQ_STORAGE_PERSISTENCE_FAILED UINT32_C(3)
#define RODAQ_STORAGE_PERSISTENCE_UNKNOWN UINT32_C(4)

/* ABI-FROZEN: zero is never a valid operation handle. */
typedef struct rodaq_storage_operation {
    uint64_t value;
} rodaq_storage_operation;

typedef uint32_t rodaq_descriptor_readiness;

#define RODAQ_DESCRIPTOR_READY UINT32_C(1)
#define RODAQ_DESCRIPTOR_UNSUPPORTED UINT32_C(2)
#define RODAQ_DESCRIPTOR_FAILED UINT32_C(3)

/* ABI-APPENDABLE: Storage capability snapshot. */
typedef struct rodaq_storage_capabilities {
    uint32_t struct_size;
    uint32_t flags;

    rodaq_bool full_fidelity;
    uint32_t reserved;

    uint64_t maximum_payload_bytes;

    uint64_t maximum_inflight_records;
    uint64_t maximum_inflight_bytes;

    uint64_t handoff_progress_timeout_ns;
    uint64_t descriptor_progress_timeout_ns;
    uint64_t flush_progress_timeout_ns;
    uint64_t close_progress_timeout_ns;
} rodaq_storage_capabilities;

/* ABI-APPENDABLE: caller-owned Descriptor readiness result. */
typedef struct rodaq_descriptor_readiness_result {
    uint32_t struct_size;
    rodaq_descriptor_readiness state;

    rodaq_status failure;
} rodaq_descriptor_readiness_result;

#define RODAQ_STORAGE_CONTROL_PROTOCOL_VERSION UINT32_C(1)

/* ABI-APPENDABLE: required Storage control interface table. */
typedef struct rodaq_storage_control_v1 {
    uint32_t protocol_version;
    uint32_t struct_size;

    rodaq_status_code (*get_capabilities)(
        void* context, rodaq_storage_capabilities* out_capabilities,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*ensure_descriptor_ready)(
        void* context, rodaq_stream_descriptor_handle descriptor,
        rodaq_descriptor_readiness_result* out_result, rodaq_status* out_status) RODAQ_NOEXCEPT;
} rodaq_storage_control_v1;

/* ABI-APPENDABLE: Storage open request. */
typedef struct rodaq_storage_open_request {
    uint32_t struct_size;
    uint32_t flags;

    rodaq_string_view session_id;
    rodaq_string_view target_uri;
} rodaq_storage_open_request;

/* ABI-APPENDABLE: Storage close request. */
typedef struct rodaq_storage_close_request {
    uint32_t struct_size;
    rodaq_storage_close_disposition disposition;
    uint64_t reserved[2];
} rodaq_storage_close_request;

/* ABI-APPENDABLE: Storage Record submission. */
typedef struct rodaq_storage_submission {
    uint32_t struct_size;
    uint32_t flags;

    rodaq_storage_operation operation;

    rodaq_stream_handle stream;
    rodaq_stream_descriptor_handle descriptor;

    uint64_t acceptance_sequence;

    rodaq_bool source_present;
    rodaq_bool source_sequence_present;

    rodaq_source_handle source;
    uint64_t source_sequence;

    const rodaq_timestamp_value* timestamp_values;
    uint32_t timestamp_value_count;
    uint32_t reserved;

    rodaq_bytes_view payload;
} rodaq_storage_submission;

/* ABI-APPENDABLE: synchronous Storage handoff result. */
typedef struct rodaq_storage_submit_result {
    uint32_t struct_size;
    uint32_t reserved;

    rodaq_storage_payload_state payload_state;
    rodaq_storage_persistence_state persistence_state;

    rodaq_status persistence_error;
} rodaq_storage_submit_result;

#define RODAQ_STORAGE_PROTOCOL_VERSION UINT32_C(1)

/* ABI-APPENDABLE: provider-owned Storage family protocol table. */
struct rodaq_storage_plugin {
    uint32_t protocol_version;
    uint32_t struct_size;

    const rodaq_plugin_descriptor* descriptor;

    rodaq_status_code (*create)(void** out_instance, rodaq_status* out_status) RODAQ_NOEXCEPT;

    void (*destroy)(void* instance) RODAQ_NOEXCEPT;

    rodaq_status_code (*bind)(
        void* instance, const rodaq_service_registry* services,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*configure)(
        void* instance, rodaq_config_view config, rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*open)(
        void* instance, const rodaq_storage_open_request* request,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*submit)(
        void* instance, const rodaq_storage_submission* submission,
        rodaq_storage_submit_result* out_result, rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*flush)(void* instance, rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*close)(
        void* instance, const rodaq_storage_close_request* request,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*query_interface)(
        void* instance, rodaq_string_view interface_id, rodaq_interface* out_interface,
        rodaq_status* out_status) RODAQ_NOEXCEPT;
};

#define RODAQ_STORAGE_CAPABILITIES_MIN_STRUCT_SIZE                     \
    (offsetof(rodaq_storage_capabilities, close_progress_timeout_ns) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_storage_capabilities, close_progress_timeout_ns))

#define RODAQ_DESCRIPTOR_READINESS_RESULT_MIN_STRUCT_SIZE   \
    (offsetof(rodaq_descriptor_readiness_result, failure) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_descriptor_readiness_result, failure))

#define RODAQ_STORAGE_CONTROL_V1_MIN_STRUCT_SIZE                   \
    (offsetof(rodaq_storage_control_v1, ensure_descriptor_ready) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_storage_control_v1, ensure_descriptor_ready))

#define RODAQ_STORAGE_OPEN_REQUEST_MIN_STRUCT_SIZE      \
    (offsetof(rodaq_storage_open_request, target_uri) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_storage_open_request, target_uri))

#define RODAQ_STORAGE_CLOSE_REQUEST_MIN_STRUCT_SIZE       \
    (offsetof(rodaq_storage_close_request, disposition) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_storage_close_request, disposition))

#define RODAQ_STORAGE_SUBMISSION_MIN_STRUCT_SIZE   \
    (offsetof(rodaq_storage_submission, payload) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_storage_submission, payload))

#define RODAQ_STORAGE_SUBMIT_RESULT_MIN_STRUCT_SIZE             \
    (offsetof(rodaq_storage_submit_result, persistence_error) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_storage_submit_result, persistence_error))

#define RODAQ_STORAGE_PLUGIN_MIN_STRUCT_SIZE           \
    (offsetof(rodaq_storage_plugin, query_interface) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_storage_plugin, query_interface))

#ifdef __cplusplus
}
#endif

#endif /* RODAQ_PLUGIN_ABI_STORAGE_H_ */
