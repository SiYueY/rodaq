#include <rodaq/c/acquisition.h>
#include <rodaq/c/storage.h>

#include <stddef.h>

/* Independent signature expectations, not derived from the production declarations. */
typedef uint32_t (*abi_version_fn)(void);
typedef const rodaq_plugin_entry_t* (*entry_fn)(void);
typedef rodaq_status_code_t (*create_fn)(rodaq_plugin_instance_t*, rodaq_error_t*);
typedef const rodaq_plugin_descriptor_t* (*descriptor_fn)(void);
typedef rodaq_status_code_t (*configure_fn)(
    rodaq_plugin_context_t*, const rodaq_application_callbacks_t*, const rodaq_config_t*,
    rodaq_error_t*);
typedef rodaq_status_code_t (*query_fn)(
    rodaq_plugin_context_t*, const rodaq_interface_query_t*, rodaq_interface_reference_t*,
    rodaq_error_t*);
typedef rodaq_status_code_t (*shutdown_fn)(
    rodaq_plugin_context_t*, rodaq_deadline_t, rodaq_error_t*);
typedef void (*destroy_fn)(rodaq_plugin_context_t*);
typedef void (*app_log_fn)(void*, rodaq_log_level_t, rodaq_string_t);
typedef rodaq_status_code_t (*acquisition_prepare_fn)(
    rodaq_interface_context_t*, const rodaq_acquisition_prepare_config_t*,
    const rodaq_acquisition_callbacks_t*, rodaq_deadline_t, rodaq_error_t*);
typedef rodaq_status_code_t (*control_fn)(
    rodaq_interface_context_t*, rodaq_deadline_t, rodaq_error_t*);
typedef rodaq_status_code_t (*cb_prepare_descriptor_fn)(
    void*, rodaq_stream_id_t, const rodaq_acquisition_descriptor_t*, rodaq_descriptor_id_t*,
    rodaq_error_t*);
typedef rodaq_record_disposition_t (*cb_submit_fn)(
    void*, const rodaq_acquisition_record_attempt_t*);
typedef void (*cb_report_event_fn)(void*, const rodaq_acquisition_event_t*);
typedef rodaq_status_code_t (*storage_prepare_fn)(
    rodaq_interface_context_t*, const rodaq_storage_prepare_config_t*, rodaq_deadline_t,
    rodaq_error_t*);
typedef rodaq_status_code_t (*prepare_descriptor_fn)(
    rodaq_interface_context_t*, const rodaq_storage_descriptor_t*, rodaq_deadline_t,
    rodaq_error_t*);
typedef rodaq_status_code_t (*open_fn)(
    rodaq_interface_context_t*, const rodaq_segment_spec_t*, rodaq_deadline_t, rodaq_error_t*);
typedef rodaq_status_code_t (*write_fn)(
    rodaq_interface_context_t*, const rodaq_storage_record_t*, uint32_t, rodaq_deadline_t,
    rodaq_storage_batch_result_t*, rodaq_error_t*);
typedef rodaq_status_code_t (*usage_fn)(
    rodaq_interface_context_t*, rodaq_segment_usage_t*, rodaq_error_t*);
typedef rodaq_status_code_t (*seal_fn)(
    rodaq_interface_context_t*, rodaq_deadline_t, rodaq_segment_seal_result_t*, rodaq_error_t*);

#ifdef __cplusplus
#include <type_traits>
#define RODAQ_CHECK_SIGNATURE(expression, type) \
    static_assert(std::is_same_v<decltype(expression), type>, "Signature mismatch: " #expression)
#define RODAQ_STATIC_ASSERT static_assert
#else
#define RODAQ_CHECK_SIGNATURE(expression, type) \
    _Static_assert(                             \
        _Generic((expression), type : 1, default : 0), "Signature mismatch: " #expression)

#define RODAQ_STATIC_ASSERT _Static_assert
#endif

RODAQ_CHECK_SIGNATURE(&rodaq_plugin_abi_version, abi_version_fn);
RODAQ_CHECK_SIGNATURE(&rodaq_plugin_get_entry, entry_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_plugin_entry_t*)0)->descriptor, descriptor_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_plugin_entry_t*)0)->create, create_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_plugin_instance_vtable_t*)0)->configure, configure_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_plugin_instance_vtable_t*)0)->query, query_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_plugin_instance_vtable_t*)0)->shutdown, shutdown_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_plugin_instance_vtable_t*)0)->destroy, destroy_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_application_callbacks_t*)0)->log, app_log_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_acquisition_interface_vtable_t*)0)->prepare, acquisition_prepare_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_acquisition_interface_vtable_t*)0)->start, control_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_acquisition_interface_vtable_t*)0)->quiesce, control_fn);
RODAQ_CHECK_SIGNATURE(
    ((rodaq_acquisition_callbacks_t*)0)->prepare_descriptor, cb_prepare_descriptor_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_acquisition_callbacks_t*)0)->submit_record, cb_submit_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_acquisition_callbacks_t*)0)->report_event, cb_report_event_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_storage_interface_vtable_t*)0)->prepare, storage_prepare_fn);
RODAQ_CHECK_SIGNATURE(
    ((rodaq_storage_interface_vtable_t*)0)->prepare_descriptor, prepare_descriptor_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_storage_interface_vtable_t*)0)->open_segment, open_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_storage_interface_vtable_t*)0)->write_batch, write_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_storage_interface_vtable_t*)0)->segment_usage, usage_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_storage_interface_vtable_t*)0)->seal_segment, seal_fn);
RODAQ_CHECK_SIGNATURE(((rodaq_storage_interface_vtable_t*)0)->quiesce, control_fn);

RODAQ_STATIC_ASSERT(sizeof(rodaq_source_id_t) == 32, "Source ID layout");
RODAQ_STATIC_ASSERT(
    sizeof(rodaq_descriptor_metadata_t) == 2 * sizeof(rodaq_string_t),
    "Descriptor metadata layout");
RODAQ_STATIC_ASSERT(
    offsetof(rodaq_plugin_descriptor_t, abi_revision) + sizeof(uint32_t) <=
        sizeof(rodaq_plugin_descriptor_t),
    "Plugin descriptor abi_revision layout");
