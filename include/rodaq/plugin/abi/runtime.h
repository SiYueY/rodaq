#ifndef RODAQ_PLUGIN_ABI_RUNTIME_H_
#define RODAQ_PLUGIN_ABI_RUNTIME_H_

#include <rodaq/plugin/abi/acquisition.h>
#include <rodaq/plugin/abi/canonical.h>
#include <rodaq/plugin/abi/config.h>
#include <rodaq/plugin/abi/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RODAQ_RUNTIME_COMMON_SERVICE_ID "rodaq.runtime.common"
#define RODAQ_RUNTIME_CANONICAL_SERVICE_ID "rodaq.runtime.canonical"
#define RODAQ_RUNTIME_PROJECTION_SERVICE_ID "rodaq.runtime.projection"
#define RODAQ_RUNTIME_CLOCK_CORRELATION_SERVICE_ID "rodaq.runtime.clock_correlation"
#define RODAQ_RUNTIME_ACQUISITION_SERVICE_ID "rodaq.runtime.acquisition"
#define RODAQ_RUNTIME_STORAGE_SERVICE_ID "rodaq.runtime.storage"

typedef uint32_t rodaq_log_level;

#define RODAQ_LOG_TRACE UINT32_C(1)
#define RODAQ_LOG_DEBUG UINT32_C(2)
#define RODAQ_LOG_INFO UINT32_C(3)
#define RODAQ_LOG_WARN UINT32_C(4)
#define RODAQ_LOG_ERROR UINT32_C(5)
#define RODAQ_LOG_CRITICAL UINT32_C(6)
#define RODAQ_LOG_OFF UINT32_C(7)

typedef uint32_t rodaq_diagnostic_severity;

#define RODAQ_DIAGNOSTIC_INFO UINT32_C(1)
#define RODAQ_DIAGNOSTIC_WARNING UINT32_C(2)
#define RODAQ_DIAGNOSTIC_ERROR UINT32_C(3)
#define RODAQ_DIAGNOSTIC_FATAL UINT32_C(4)

typedef uint32_t rodaq_runtime_lifecycle_state;

#define RODAQ_RUNTIME_CONFIGURED UINT32_C(1)
#define RODAQ_RUNTIME_STARTING UINT32_C(2)
#define RODAQ_RUNTIME_RUNNING UINT32_C(3)
#define RODAQ_RUNTIME_STOPPING UINT32_C(4)
#define RODAQ_RUNTIME_STOPPED UINT32_C(5)
#define RODAQ_RUNTIME_FAILED UINT32_C(6)

/* ABI-FROZEN: one DAQ lifecycle observation. */
typedef struct rodaq_runtime_lifecycle_snapshot {
    rodaq_runtime_lifecycle_state state;
    rodaq_bool normal_acquisition_allowed;
    uint64_t generation;
} rodaq_runtime_lifecycle_snapshot;

#define RODAQ_RUNTIME_COMMON_PROTOCOL_VERSION UINT32_C(1)

/* ABI-APPENDABLE: DAQ common service table. */
typedef struct rodaq_runtime_common {
    uint32_t protocol_version;
    uint32_t struct_size;

    void (*log)(
        void* context, rodaq_log_level level, rodaq_string_view category,
        rodaq_string_view message) RODAQ_NOEXCEPT;

    uint64_t (*monotonic_time_ns)(void* context) RODAQ_NOEXCEPT;

    rodaq_status_code (*get_lifecycle_snapshot)(
        void* context, rodaq_runtime_lifecycle_snapshot* out_snapshot,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    void (*report_diagnostic)(
        void* context, rodaq_diagnostic_severity severity, rodaq_string_view code,
        rodaq_string_view message) RODAQ_NOEXCEPT;
} rodaq_runtime_common;

#define RODAQ_RUNTIME_CANONICAL_PROTOCOL_VERSION UINT32_C(1)

/* ABI-APPENDABLE: read-only Canonical DAQ service table. */
typedef struct rodaq_runtime_canonical {
    uint32_t protocol_version;
    uint32_t struct_size;

    rodaq_status_code (*get_source)(
        void* context, rodaq_source_handle handle, rodaq_source_view* out_view,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*get_clock)(
        void* context, rodaq_clock_handle handle, rodaq_clock_view* out_view,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*get_schema)(
        void* context, rodaq_schema_handle handle, rodaq_schema_view* out_view,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*get_timestamp_key_set)(
        void* context, uint64_t timestamp_key_set_id, rodaq_timestamp_key_set_view* out_view,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*get_stream)(
        void* context, rodaq_stream_handle handle, rodaq_stream_view* out_view,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*get_stream_descriptor)(
        void* context, rodaq_stream_descriptor_handle handle,
        rodaq_stream_descriptor_view* out_view, rodaq_status* out_status) RODAQ_NOEXCEPT;
} rodaq_runtime_canonical;

#define RODAQ_RUNTIME_PROJECTION_PROTOCOL_VERSION UINT32_C(1)

/* ABI-APPENDABLE: one immutable typed interoperability projection. */
typedef struct rodaq_projection_registration {
    uint32_t struct_size;
    uint32_t reserved;

    rodaq_stream_descriptor_handle descriptor;
    rodaq_string_view projection_id;
    rodaq_bytes_view payload;
} rodaq_projection_registration;

/* ABI-APPENDABLE: borrowed immutable projection data. */
typedef struct rodaq_projection_view {
    uint32_t struct_size;
    uint32_t reserved;

    rodaq_stream_descriptor_handle descriptor;
    rodaq_string_view projection_id;
    rodaq_bytes_view payload;
} rodaq_projection_view;

/* ABI-APPENDABLE: projection DAQ service table. */
typedef struct rodaq_runtime_projection {
    uint32_t protocol_version;
    uint32_t struct_size;

    rodaq_status_code (*register_projection)(
        void* context, const rodaq_projection_registration* request,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*get_projection)(
        void* context, rodaq_stream_descriptor_handle descriptor, rodaq_string_view projection_id,
        rodaq_projection_view* out_view, rodaq_status* out_status) RODAQ_NOEXCEPT;
} rodaq_runtime_projection;

/* ABI-APPENDABLE: activation-scoped ingress Clock to Unix epoch correlation. */
typedef struct rodaq_unix_epoch_clock_anchor {
    uint32_t struct_size;
    uint32_t reserved;

    rodaq_clock_handle clock;
    int64_t clock_reading;
    uint64_t unix_anchor_ns;
} rodaq_unix_epoch_clock_anchor;

#define RODAQ_RUNTIME_CLOCK_CORRELATION_PROTOCOL_VERSION UINT32_C(1)

/* ABI-APPENDABLE: read-only clock-correlation DAQ service table. */
typedef struct rodaq_runtime_clock_correlation {
    uint32_t protocol_version;
    uint32_t struct_size;

    rodaq_status_code (*get_unix_epoch_anchor)(
        void* context, rodaq_clock_handle clock, rodaq_unix_epoch_clock_anchor* out_anchor,
        rodaq_status* out_status) RODAQ_NOEXCEPT;
} rodaq_runtime_clock_correlation;

#define RODAQ_RUNTIME_ACQUISITION_PROTOCOL_VERSION UINT32_C(1)

/* ABI-APPENDABLE: Acquisition DAQ service table. */
typedef struct rodaq_runtime_acquisition {
    uint32_t protocol_version;
    uint32_t struct_size;

    rodaq_status_code (*register_source)(
        void* context, const rodaq_source_registration* request, rodaq_source_handle* out_handle,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*register_clock)(
        void* context, const rodaq_clock_registration* request, rodaq_clock_handle* out_handle,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*register_schema)(
        void* context, const rodaq_schema_registration* request, rodaq_schema_handle* out_handle,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*register_stream)(
        void* context, const rodaq_stream_registration* request, rodaq_stream_handle* out_handle,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*publish_stream_descriptor)(
        void* context, const rodaq_stream_descriptor_registration* request,
        rodaq_stream_descriptor_handle* out_handle, rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*report_source_availability)(
        void* context, rodaq_source_handle source, rodaq_availability_state state,
        const rodaq_status* diagnostic, rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*report_stream_availability)(
        void* context, rodaq_stream_handle stream, rodaq_availability_state state,
        const rodaq_status* diagnostic, rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*report_plugin_state)(
        void* context, rodaq_acquisition_plugin_state state, const rodaq_status* diagnostic,
        rodaq_status* out_status) RODAQ_NOEXCEPT;

    rodaq_status_code (*submit)(
        void* context, const rodaq_record_submission* submission,
        rodaq_acquisition_submit_result* out_result, rodaq_status* out_status) RODAQ_NOEXCEPT;
} rodaq_runtime_acquisition;

#define RODAQ_RUNTIME_STORAGE_PROTOCOL_VERSION UINT32_C(1)

/* ABI-APPENDABLE: Storage DAQ completion service table. */
typedef struct rodaq_runtime_storage {
    uint32_t protocol_version;
    uint32_t struct_size;

    void (*payload_consumed)(void* context, rodaq_storage_operation operation) RODAQ_NOEXCEPT;

    void (*persistence_completed)(
        void* context, rodaq_storage_operation operation,
        rodaq_storage_persistence_state terminal_state, const rodaq_status* error) RODAQ_NOEXCEPT;

    void (*report_failure)(void* context, const rodaq_status* error) RODAQ_NOEXCEPT;
} rodaq_runtime_storage;

#define RODAQ_RUNTIME_COMMON_MIN_STRUCT_SIZE             \
    (offsetof(rodaq_runtime_common, report_diagnostic) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_runtime_common, report_diagnostic))

#define RODAQ_RUNTIME_CANONICAL_MIN_STRUCT_SIZE                 \
    (offsetof(rodaq_runtime_canonical, get_stream_descriptor) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_runtime_canonical, get_stream_descriptor))

#define RODAQ_PROJECTION_REGISTRATION_MIN_STRUCT_SIZE   \
    (offsetof(rodaq_projection_registration, payload) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_projection_registration, payload))

#define RODAQ_PROJECTION_VIEW_MIN_STRUCT_SIZE   \
    (offsetof(rodaq_projection_view, payload) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_projection_view, payload))

#define RODAQ_RUNTIME_PROJECTION_MIN_STRUCT_SIZE          \
    (offsetof(rodaq_runtime_projection, get_projection) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_runtime_projection, get_projection))

#define RODAQ_UNIX_EPOCH_CLOCK_ANCHOR_MIN_STRUCT_SIZE          \
    (offsetof(rodaq_unix_epoch_clock_anchor, unix_anchor_ns) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_unix_epoch_clock_anchor, unix_anchor_ns))

#define RODAQ_RUNTIME_CLOCK_CORRELATION_MIN_STRUCT_SIZE                 \
    (offsetof(rodaq_runtime_clock_correlation, get_unix_epoch_anchor) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_runtime_clock_correlation, get_unix_epoch_anchor))

#define RODAQ_RUNTIME_ACQUISITION_MIN_STRUCT_SIZE  \
    (offsetof(rodaq_runtime_acquisition, submit) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_runtime_acquisition, submit))

#define RODAQ_RUNTIME_STORAGE_MIN_STRUCT_SIZE          \
    (offsetof(rodaq_runtime_storage, report_failure) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_runtime_storage, report_failure))

#ifdef __cplusplus
}
#endif

#endif /* RODAQ_PLUGIN_ABI_RUNTIME_H_ */
