#ifndef RODAQ_PLUGIN_ABI_ACQUISITION_H_
#define RODAQ_PLUGIN_ABI_ACQUISITION_H_

#include <rodaq/plugin/abi/canonical.h>
#include <rodaq/plugin/abi/config.h>
#include <rodaq/plugin/abi/plugin.h>
#include <rodaq/plugin/abi/service.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RODAQ_ACQUISITION_CONTROL_INTERFACE_ID "rodaq.acquisition.control.v1"

typedef uint32_t rodaq_payload_mode;

#define RODAQ_PAYLOAD_COPY UINT32_C(1)
#define RODAQ_PAYLOAD_ADOPT UINT32_C(2)
#define RODAQ_PAYLOAD_RETAIN UINT32_C(3)

typedef uint32_t rodaq_payload_disposition;

#define RODAQ_PAYLOAD_DISPOSITION_NOT_APPLICABLE UINT32_C(0)
#define RODAQ_PAYLOAD_DISPOSITION_CALLER_RESPONSIBLE UINT32_C(1)
#define RODAQ_PAYLOAD_DISPOSITION_RUNTIME_RESPONSIBLE UINT32_C(2)

typedef uint32_t rodaq_admission_outcome;

#define RODAQ_ADMISSION_PRE_REJECTED UINT32_C(1)
#define RODAQ_ADMISSION_REJECTED UINT32_C(2)
#define RODAQ_ADMISSION_ACCEPTED UINT32_C(3)

typedef uint32_t rodaq_admission_rejection_reason;

#define RODAQ_REJECTION_NONE UINT32_C(0)
#define RODAQ_REJECTION_INVALID_INPUT UINT32_C(1)
#define RODAQ_REJECTION_RUNTIME_NOT_RUNNING UINT32_C(2)
#define RODAQ_REJECTION_GATE_CLOSED UINT32_C(3)
#define RODAQ_REJECTION_STALE_HANDLE UINT32_C(4)
#define RODAQ_REJECTION_BINDING_NOT_ACTIVE UINT32_C(5)
#define RODAQ_REJECTION_BINDING_NOT_CURRENT UINT32_C(6)
#define RODAQ_REJECTION_STREAM_UNAVAILABLE UINT32_C(7)
#define RODAQ_REJECTION_SOURCE_UNAVAILABLE UINT32_C(8)
#define RODAQ_REJECTION_SEQUENCE_EXHAUSTED UINT32_C(9)
#define RODAQ_REJECTION_PAYLOAD_INVALID UINT32_C(10)
#define RODAQ_REJECTION_PAYLOAD_TOO_LARGE UINT32_C(11)
#define RODAQ_REJECTION_RESOURCE_UNAVAILABLE UINT32_C(12)
#define RODAQ_REJECTION_WAIT_TIMEOUT UINT32_C(13)
#define RODAQ_REJECTION_RUNTIME_FAILURE UINT32_C(14)

typedef uint32_t rodaq_availability_state;

#define RODAQ_AVAILABILITY_AVAILABLE UINT32_C(1)
#define RODAQ_AVAILABILITY_UNAVAILABLE UINT32_C(2)

typedef uint32_t rodaq_acquisition_plugin_state;

#define RODAQ_ACQUISITION_PLUGIN_AVAILABLE UINT32_C(1)
#define RODAQ_ACQUISITION_PLUGIN_RECOVERABLE_FAILURE UINT32_C(2)
#define RODAQ_ACQUISITION_PLUGIN_TERMINAL_FAILURE UINT32_C(3)

/* ABI-FROZEN: Payload input and its submission-scoped lifetime anchor. */
typedef struct rodaq_payload_input {
    rodaq_payload_mode mode;
    uint32_t reserved;

    const uint8_t* data;
    uint64_t size;

    void* owner_context;

    void (*release)(void* owner_context) RODAQ_NOEXCEPT;
} rodaq_payload_input;

/* ABI-APPENDABLE: Acquisition Record submission. */
typedef struct rodaq_record_submission {
    uint32_t struct_size;
    uint32_t flags;

    rodaq_stream_handle stream;
    rodaq_stream_descriptor_handle descriptor;

    rodaq_bool source_present;
    rodaq_bool source_sequence_present;

    rodaq_source_handle source;
    uint64_t source_sequence;

    const rodaq_timestamp_value* timestamp_values;
    uint32_t timestamp_value_count;
    uint32_t reserved;

    rodaq_payload_input payload;
} rodaq_record_submission;

/* ABI-APPENDABLE: terminal Admission result for one submit call. */
typedef struct rodaq_acquisition_submit_result {
    uint32_t struct_size;
    rodaq_admission_outcome outcome;

    rodaq_admission_rejection_reason rejection_reason;
    rodaq_payload_disposition payload_disposition;

    uint64_t reserved[2];
} rodaq_acquisition_submit_result;

/* ABI-FROZEN: activation-lifetime Binding transition token. */
typedef struct rodaq_binding_transition {
    uint64_t value;
} rodaq_binding_transition;

typedef uint32_t rodaq_binding_abort_disposition;

#define RODAQ_BINDING_ABORT_KEEP_PAUSED UINT32_C(1)
#define RODAQ_BINDING_ABORT_RESUME_OLD UINT32_C(2)

#define RODAQ_ACQUISITION_CONTROL_PROTOCOL_VERSION UINT32_C(1)

/* ABI-APPENDABLE: required Acquisition control interface table. */
typedef struct rodaq_acquisition_control_v1 {
    uint32_t protocol_version;
    uint32_t struct_size;

    rodaq_status (*prepare_binding_switch)(
        void* context, rodaq_binding_transition transition, rodaq_stream_handle stream,
        rodaq_stream_descriptor_handle old_descriptor,
        rodaq_stream_descriptor_handle new_descriptor, rodaq_error* out_error) RODAQ_NOEXCEPT;

    rodaq_status (*commit_binding_switch)(
        void* context, rodaq_binding_transition transition, rodaq_error* out_error) RODAQ_NOEXCEPT;

    rodaq_status (*abort_binding_switch)(
        void* context, rodaq_binding_transition transition,
        rodaq_binding_abort_disposition disposition, rodaq_error* out_error) RODAQ_NOEXCEPT;

    rodaq_status (*quiesce_stream)(
        void* context, rodaq_stream_handle stream, rodaq_error* out_error) RODAQ_NOEXCEPT;

    rodaq_status (*quiesce_plugin)(void* context, rodaq_error* out_error) RODAQ_NOEXCEPT;
} rodaq_acquisition_control_v1;

#define RODAQ_ACQUISITION_PROTOCOL_VERSION UINT32_C(1)

/* ABI-APPENDABLE: provider-owned Acquisition family protocol table. */
struct rodaq_acquisition_plugin {
    uint32_t protocol_version;
    uint32_t struct_size;

    const rodaq_plugin_descriptor* descriptor;

    rodaq_status (*create)(void** out_instance, rodaq_error* out_error) RODAQ_NOEXCEPT;

    void (*destroy)(void* instance) RODAQ_NOEXCEPT;

    rodaq_status (*bind)(
        void* instance, const rodaq_service_registry* services,
        rodaq_error* out_error) RODAQ_NOEXCEPT;

    rodaq_status (*configure)(void* instance, rodaq_config_view config, rodaq_error* out_error)
        RODAQ_NOEXCEPT;

    rodaq_status (*start)(void* instance, rodaq_error* out_error) RODAQ_NOEXCEPT;

    rodaq_status (*stop)(void* instance, rodaq_error* out_error) RODAQ_NOEXCEPT;

    rodaq_status (*query_interface)(
        void* instance, rodaq_string_view interface_id, rodaq_interface* out_interface,
        rodaq_error* out_error) RODAQ_NOEXCEPT;
};

#define RODAQ_RECORD_SUBMISSION_MIN_STRUCT_SIZE   \
    (offsetof(rodaq_record_submission, payload) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_record_submission, payload))

#define RODAQ_ACQUISITION_SUBMIT_RESULT_MIN_STRUCT_SIZE               \
    (offsetof(rodaq_acquisition_submit_result, payload_disposition) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_acquisition_submit_result, payload_disposition))

#define RODAQ_ACQUISITION_CONTROL_V1_MIN_STRUCT_SIZE          \
    (offsetof(rodaq_acquisition_control_v1, quiesce_plugin) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_acquisition_control_v1, quiesce_plugin))

#define RODAQ_ACQUISITION_PLUGIN_MIN_STRUCT_SIZE           \
    (offsetof(rodaq_acquisition_plugin, query_interface) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_acquisition_plugin, query_interface))

#ifdef __cplusplus
}
#endif

#endif /* RODAQ_PLUGIN_ABI_ACQUISITION_H_ */
