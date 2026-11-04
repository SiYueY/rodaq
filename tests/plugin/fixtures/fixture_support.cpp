#include "fixture_support.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>

#ifndef RODAQ_FIXTURE_CONTROL_PROTOCOL_VERSION
#define RODAQ_FIXTURE_CONTROL_PROTOCOL_VERSION RODAQ_ACQUISITION_CONTROL_PROTOCOL_VERSION
#endif

#ifndef RODAQ_FIXTURE_CONTROL_STRUCT_SIZE
#define RODAQ_FIXTURE_CONTROL_STRUCT_SIZE RODAQ_ACQUISITION_CONTROL_V1_MIN_STRUCT_SIZE
#endif

#ifndef RODAQ_FIXTURE_CONTROL_APPENDABLE_TAIL
#define RODAQ_FIXTURE_CONTROL_APPENDABLE_TAIL 0
#endif

#ifndef RODAQ_FIXTURE_CONTROL_MISSING_SLOT
#define RODAQ_FIXTURE_CONTROL_MISSING_SLOT 0
#endif

#ifndef RODAQ_FIXTURE_CONTROL_NULL_CONTEXT
#define RODAQ_FIXTURE_CONTROL_NULL_CONTEXT 0
#endif

#ifndef RODAQ_FIXTURE_STORAGE_CONTROL_PROTOCOL_VERSION
#define RODAQ_FIXTURE_STORAGE_CONTROL_PROTOCOL_VERSION RODAQ_STORAGE_CONTROL_PROTOCOL_VERSION
#endif

#ifndef RODAQ_FIXTURE_STORAGE_CONTROL_STRUCT_SIZE
#define RODAQ_FIXTURE_STORAGE_CONTROL_STRUCT_SIZE RODAQ_STORAGE_CONTROL_V1_MIN_STRUCT_SIZE
#endif

#ifndef RODAQ_FIXTURE_STORAGE_CONTROL_MISSING_SLOT
#define RODAQ_FIXTURE_STORAGE_CONTROL_MISSING_SLOT 0
#endif

namespace rodaq::test {
namespace {

class FixtureLifecycleObserver final {
public:
    FixtureLifecycleObserver() { fixture_record_event("load"); }

    ~FixtureLifecycleObserver() { fixture_record_event("unload"); }
};

FixtureLifecycleObserver lifecycle_observer;

struct FixtureInstanceState {
    bool bound = false;
    bool configured = false;
    std::int64_t limit = 0;
    char mode[32]{};
};

FixtureInstanceState instance_storage;

rodaq_status fail(rodaq_status status, const char* message, rodaq_error* out_error) noexcept {
    if (out_error != nullptr) {
        *out_error = {};
        out_error->code = status;
        const std::string_view domain("fixture");
        const std::string_view text(message);
        out_error->domain_size = static_cast<std::uint32_t>(domain.size());
        out_error->message_size = static_cast<std::uint32_t>(text.size());
        std::memcpy(out_error->domain, domain.data(), domain.size());
        std::memcpy(out_error->message, text.data(), text.size());
    }
    return status;
}

rodaq_status query_service(
    const rodaq_service_registry* services, const char* service_id, rodaq_interface* out_service,
    rodaq_error* out_error) noexcept {
    if (services == nullptr || services->context == nullptr || services->vtable == nullptr ||
        services->vtable->query == nullptr) {
        return fail(RODAQ_STATUS_INVALID_ARGUMENT, "invalid Service Registry", out_error);
    }
    return services->vtable->query(
        services->context,
        rodaq_string_view{service_id, static_cast<std::uint64_t>(std::strlen(service_id))},
        out_service, out_error);
}

template <typename Table>
rodaq_status validate_table(
    const rodaq_interface& service, std::uint32_t protocol_version, std::uint32_t minimum_size,
    rodaq_error* out_error) noexcept {
    const auto* table = static_cast<const Table*>(service.vtable);
    if (service.context == nullptr || table == nullptr ||
        table->protocol_version != protocol_version || table->struct_size < minimum_size) {
        return fail(
            RODAQ_STATUS_INVALID_ARGUMENT, "required Runtime service ABI is invalid", out_error);
    }
    return RODAQ_STATUS_OK;
}

rodaq_status validate_common_service(
    const rodaq_interface& service, rodaq_error* out_error) noexcept {
    const auto* table = static_cast<const rodaq_runtime_common_v1*>(service.vtable);
    rodaq_status status = validate_table<rodaq_runtime_common_v1>(
        service, RODAQ_RUNTIME_COMMON_PROTOCOL_VERSION, RODAQ_RUNTIME_COMMON_V1_MIN_STRUCT_SIZE,
        out_error);
    if (status != RODAQ_STATUS_OK) {
        return status;
    }
    if (table->log == nullptr || table->monotonic_time_ns == nullptr ||
        table->get_lifecycle_snapshot == nullptr || table->report_diagnostic == nullptr) {
        return fail(
            RODAQ_STATUS_INVALID_ARGUMENT, "common Runtime service slot is null", out_error);
    }
    return RODAQ_STATUS_OK;
}

rodaq_status validate_canonical_service(
    const rodaq_interface& service, rodaq_error* out_error) noexcept {
    const auto* table = static_cast<const rodaq_runtime_canonical_v1*>(service.vtable);
    rodaq_status status = validate_table<rodaq_runtime_canonical_v1>(
        service, RODAQ_RUNTIME_CANONICAL_PROTOCOL_VERSION,
        RODAQ_RUNTIME_CANONICAL_V1_MIN_STRUCT_SIZE, out_error);
    if (status != RODAQ_STATUS_OK) {
        return status;
    }
    if (table->get_source == nullptr || table->get_clock == nullptr ||
        table->get_schema == nullptr || table->get_timestamp_key_set == nullptr ||
        table->get_stream == nullptr || table->get_stream_descriptor == nullptr) {
        return fail(
            RODAQ_STATUS_INVALID_ARGUMENT, "canonical Runtime service slot is null", out_error);
    }
    return RODAQ_STATUS_OK;
}

rodaq_status validate_projection_service(
    const rodaq_interface& service, rodaq_error* out_error) noexcept {
    const auto* table = static_cast<const rodaq_runtime_projection_v1*>(service.vtable);
    rodaq_status status = validate_table<rodaq_runtime_projection_v1>(
        service, RODAQ_RUNTIME_PROJECTION_PROTOCOL_VERSION,
        RODAQ_RUNTIME_PROJECTION_V1_MIN_STRUCT_SIZE, out_error);
    if (status != RODAQ_STATUS_OK) {
        return status;
    }
    if (table->register_projection == nullptr || table->get_projection == nullptr) {
        return fail(
            RODAQ_STATUS_INVALID_ARGUMENT, "projection Runtime service slot is null", out_error);
    }
    return RODAQ_STATUS_OK;
}

rodaq_status validate_acquisition_service(
    const rodaq_interface& service, rodaq_error* out_error) noexcept {
    const auto* table = static_cast<const rodaq_runtime_acquisition_v1*>(service.vtable);
    rodaq_status status = validate_table<rodaq_runtime_acquisition_v1>(
        service, RODAQ_RUNTIME_ACQUISITION_PROTOCOL_VERSION,
        RODAQ_RUNTIME_ACQUISITION_V1_MIN_STRUCT_SIZE, out_error);
    if (status != RODAQ_STATUS_OK) {
        return status;
    }
    if (table->register_source == nullptr || table->register_clock == nullptr ||
        table->register_schema == nullptr || table->register_stream == nullptr ||
        table->publish_stream_descriptor == nullptr ||
        table->report_source_availability == nullptr ||
        table->report_stream_availability == nullptr || table->report_plugin_state == nullptr ||
        table->submit == nullptr) {
        return fail(
            RODAQ_STATUS_INVALID_ARGUMENT, "Acquisition Runtime service slot is null", out_error);
    }
    return RODAQ_STATUS_OK;
}

rodaq_status validate_storage_service(
    const rodaq_interface& service, rodaq_error* out_error) noexcept {
    const auto* table = static_cast<const rodaq_runtime_storage_v1*>(service.vtable);
    rodaq_status status = validate_table<rodaq_runtime_storage_v1>(
        service, RODAQ_RUNTIME_STORAGE_PROTOCOL_VERSION, RODAQ_RUNTIME_STORAGE_V1_MIN_STRUCT_SIZE,
        out_error);
    if (status != RODAQ_STATUS_OK) {
        return status;
    }
    if (table->payload_consumed == nullptr || table->persistence_completed == nullptr ||
        table->report_failure == nullptr) {
        return fail(
            RODAQ_STATUS_INVALID_ARGUMENT, "Storage Runtime service slot is null", out_error);
    }
    return RODAQ_STATUS_OK;
}

rodaq_status validate_clock_service(
    const rodaq_interface& service, rodaq_error* out_error) noexcept {
    const auto* table = static_cast<const rodaq_runtime_clock_correlation_v1*>(service.vtable);
    rodaq_status status = validate_table<rodaq_runtime_clock_correlation_v1>(
        service, RODAQ_RUNTIME_CLOCK_CORRELATION_PROTOCOL_VERSION,
        RODAQ_RUNTIME_CLOCK_CORRELATION_V1_MIN_STRUCT_SIZE, out_error);
    if (status != RODAQ_STATUS_OK) {
        return status;
    }
    if (table->get_unix_epoch_anchor == nullptr) {
        return fail(
            RODAQ_STATUS_INVALID_ARGUMENT, "clock-correlation service slot is null", out_error);
    }
    return RODAQ_STATUS_OK;
}

rodaq_status require_service(
    const rodaq_service_registry* services, const char* service_id, rodaq_interface* out_service,
    rodaq_error* out_error) noexcept {
    rodaq_status status = query_service(services, service_id, out_service, out_error);
    if (status != RODAQ_STATUS_OK) {
        return status;
    }
    if (out_service->context == nullptr || out_service->vtable == nullptr) {
        return fail(RODAQ_STATUS_UNAVAILABLE, "required Runtime service is unavailable", out_error);
    }
    return RODAQ_STATUS_OK;
}

rodaq_status validate_fixture_config(
    rodaq_config_view config, FixtureInstanceState* instance, rodaq_error* out_error) noexcept {
    if (config.context == nullptr || config.vtable == nullptr ||
        config.vtable->kind(config.context) != RODAQ_CONFIG_MAP) {
        return fail(RODAQ_STATUS_INVALID_ARGUMENT, "fixture config must be a MAP", out_error);
    }

    const auto find = [&config](const char* key, rodaq_config_view* out) noexcept {
        return config.vtable->find(
            config.context, rodaq_string_view{key, static_cast<std::uint64_t>(std::strlen(key))},
            out, nullptr);
    };

    rodaq_config_view enabled{};
    if (find("enabled", &enabled) != RODAQ_STATUS_OK) {
        return fail(RODAQ_STATUS_INVALID_ARGUMENT, "fixture config is missing enabled", out_error);
    }
    rodaq_bool enabled_value = RODAQ_FALSE;
    if (enabled.vtable->as_bool(enabled.context, &enabled_value, nullptr) != RODAQ_STATUS_OK ||
        enabled_value != RODAQ_TRUE) {
        return fail(RODAQ_STATUS_INVALID_ARGUMENT, "fixture enabled is invalid", out_error);
    }

    rodaq_config_view limit{};
    std::int64_t limit_value = 0;
    if (find("limit", &limit) != RODAQ_STATUS_OK ||
        limit.vtable->as_int64(limit.context, &limit_value, nullptr) != RODAQ_STATUS_OK ||
        limit_value != 7) {
        return fail(RODAQ_STATUS_INVALID_ARGUMENT, "fixture limit is invalid", out_error);
    }

    rodaq_config_view gain{};
    double gain_value = 0.0;
    if (find("gain", &gain) != RODAQ_STATUS_OK ||
        gain.vtable->as_double(gain.context, &gain_value, nullptr) != RODAQ_STATUS_OK ||
        gain_value != 0.25) {
        return fail(RODAQ_STATUS_INVALID_ARGUMENT, "fixture gain is invalid", out_error);
    }

    rodaq_config_view mode{};
    rodaq_string_view mode_value{};
    if (find("mode", &mode) != RODAQ_STATUS_OK ||
        mode.vtable->as_string(mode.context, &mode_value, nullptr) != RODAQ_STATUS_OK ||
        mode_value.size >= sizeof(instance->mode) ||
        std::string_view(mode_value.data, mode_value.size) != "fixture") {
        return fail(RODAQ_STATUS_INVALID_ARGUMENT, "fixture mode is invalid", out_error);
    }
    std::memcpy(instance->mode, mode_value.data, mode_value.size);
    instance->mode[mode_value.size] = '\0';

    rodaq_config_view nested{};
    rodaq_config_view nested_name{};
    rodaq_string_view nested_name_value{};
    if (find("nested", &nested) != RODAQ_STATUS_OK ||
        nested.vtable->kind(nested.context) != RODAQ_CONFIG_MAP ||
        nested.vtable->find(nested.context, rodaq_string_view{"name", 4}, &nested_name, nullptr) !=
            RODAQ_STATUS_OK ||
        nested_name.vtable->as_string(nested_name.context, &nested_name_value, nullptr) !=
            RODAQ_STATUS_OK ||
        std::string_view(nested_name_value.data, nested_name_value.size) != "node") {
        return fail(RODAQ_STATUS_INVALID_ARGUMENT, "fixture nested config is invalid", out_error);
    }

    rodaq_config_view items{};
    rodaq_config_view third{};
    rodaq_string_view third_value{};
    if (find("items", &items) != RODAQ_STATUS_OK ||
        items.vtable->kind(items.context) != RODAQ_CONFIG_LIST ||
        items.vtable->size(items.context) != 3 ||
        items.vtable->at(items.context, 2, &third, nullptr) != RODAQ_STATUS_OK ||
        third.vtable->as_string(third.context, &third_value, nullptr) != RODAQ_STATUS_OK ||
        std::string_view(third_value.data, third_value.size) != "three") {
        return fail(RODAQ_STATUS_INVALID_ARGUMENT, "fixture list config is invalid", out_error);
    }

    instance->limit = limit_value;
    instance->configured = true;
    return RODAQ_STATUS_OK;
}

bool string_equals(
    rodaq_string_view value, const char* expected, std::uint64_t expected_size) noexcept {
    if (value.data == nullptr || value.size != expected_size) {
        return false;
    }
    return std::char_traits<char>::compare(value.data, expected, expected_size) == 0;
}

rodaq_status prepare_binding_switch(
    void*, rodaq_binding_transition, rodaq_stream_handle, rodaq_stream_descriptor_handle,
    rodaq_stream_descriptor_handle, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status commit_binding_switch(void*, rodaq_binding_transition, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status abort_binding_switch(
    void*, rodaq_binding_transition, rodaq_binding_abort_disposition, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status quiesce_stream(void*, rodaq_stream_handle, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

[[maybe_unused]] rodaq_status quiesce_plugin(void*, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

struct ExtendedAcquisitionControl {
    rodaq_acquisition_control_v1 base;
    std::uint64_t tail;
};

const ExtendedAcquisitionControl acquisition_control = {
    {
        RODAQ_FIXTURE_CONTROL_PROTOCOL_VERSION,
        RODAQ_FIXTURE_CONTROL_APPENDABLE_TAIL ? sizeof(ExtendedAcquisitionControl)
                                              : RODAQ_FIXTURE_CONTROL_STRUCT_SIZE,
        &prepare_binding_switch,
        &commit_binding_switch,
        &abort_binding_switch,
        &quiesce_stream,
#if RODAQ_FIXTURE_CONTROL_MISSING_SLOT
        nullptr,
#else
        &quiesce_plugin,
#endif
    },
    0,
};

rodaq_status get_capabilities(
    void*, rodaq_storage_capabilities* out_capabilities, rodaq_error*) noexcept {
    if (out_capabilities == nullptr ||
        out_capabilities->struct_size < RODAQ_STORAGE_CAPABILITIES_MIN_STRUCT_SIZE) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const std::uint32_t capacity = out_capabilities->struct_size;
    const std::uint32_t initialized = static_cast<std::uint32_t>(
        std::min<std::size_t>(capacity, sizeof(rodaq_storage_capabilities)));
    std::memset(out_capabilities, 0, initialized);
    out_capabilities->struct_size = initialized;
    out_capabilities->full_fidelity = RODAQ_TRUE;
    out_capabilities->maximum_payload_bytes = 1024;
    out_capabilities->maximum_inflight_records = 8;
    out_capabilities->maximum_inflight_bytes = 8192;
    out_capabilities->handoff_progress_timeout_ns = 1'000'000;
    out_capabilities->descriptor_progress_timeout_ns = 1'000'000;
    out_capabilities->flush_progress_timeout_ns = 1'000'000;
    out_capabilities->close_progress_timeout_ns = 1'000'000;
    return RODAQ_STATUS_OK;
}

[[maybe_unused]] rodaq_status ensure_descriptor_ready(
    void*, rodaq_stream_descriptor_handle descriptor, rodaq_descriptor_readiness_result* out_result,
    rodaq_error*) noexcept {
    if (out_result == nullptr ||
        out_result->struct_size < RODAQ_DESCRIPTOR_READINESS_RESULT_MIN_STRUCT_SIZE) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const std::uint32_t capacity = out_result->struct_size;
    const std::uint32_t initialized = static_cast<std::uint32_t>(
        std::min<std::size_t>(capacity, sizeof(rodaq_descriptor_readiness_result)));
    std::memset(out_result, 0, initialized);
    out_result->struct_size = initialized;
    out_result->state =
        descriptor.value == 0 ? RODAQ_DESCRIPTOR_UNSUPPORTED : RODAQ_DESCRIPTOR_READY;
    return RODAQ_STATUS_OK;
}

const rodaq_storage_control_v1 storage_control = {
    RODAQ_FIXTURE_STORAGE_CONTROL_PROTOCOL_VERSION,
    RODAQ_FIXTURE_STORAGE_CONTROL_STRUCT_SIZE,
    &get_capabilities,
#if RODAQ_FIXTURE_STORAGE_CONTROL_MISSING_SLOT
    nullptr,
#else
    &ensure_descriptor_ready,
#endif
};

}  // namespace

void fixture_record_event(const char* event) noexcept {
    const char* path = std::getenv("RODAQ_TEST_EVENT_LOG");
    if (path == nullptr || *path == '\0') {
        return;
    }

    const int descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return;
    }
    const ssize_t event_bytes = write(descriptor, event, std::strlen(event));
    const ssize_t newline_bytes = write(descriptor, "\n", 1);
    static_cast<void>(event_bytes);
    static_cast<void>(newline_bytes);
    static_cast<void>(close(descriptor));
}

rodaq_status fixture_create(void** out_instance, rodaq_error*) noexcept {
    fixture_record_event("create");
    instance_storage = {};
    *out_instance = &instance_storage;
    return RODAQ_STATUS_OK;
}

void fixture_destroy(void*) noexcept { fixture_record_event("destroy"); }

rodaq_status fixture_create_failure(void** out_instance, rodaq_error* out_error) noexcept {
    fixture_record_event("create-failure");
    *out_instance = nullptr;
    if (out_error != nullptr) {
        *out_error = {};
        out_error->code = RODAQ_STATUS_UNAVAILABLE;
        out_error->native_code = 77;
        const char domain[] = "fixture";
        const char message[] = "fixture create failure";
        out_error->domain_size = sizeof(domain) - 1;
        out_error->message_size = sizeof(message) - 1;
        std::memcpy(out_error->domain, domain, sizeof(domain) - 1);
        std::memcpy(out_error->message, message, sizeof(message) - 1);
    }
    return RODAQ_STATUS_UNAVAILABLE;
}

rodaq_status fixture_create_null(void** out_instance, rodaq_error*) noexcept {
    fixture_record_event("create-null");
    *out_instance = nullptr;
    return RODAQ_STATUS_OK;
}

rodaq_status fixture_acquisition_bind(
    void* instance, const rodaq_service_registry* services, rodaq_error* out_error) noexcept {
    fixture_record_event("bind");
    rodaq_interface service{};
    rodaq_status status =
        require_service(services, RODAQ_RUNTIME_COMMON_SERVICE_ID, &service, out_error);
    if (status == RODAQ_STATUS_OK) {
        status = validate_common_service(service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status = require_service(services, RODAQ_RUNTIME_CANONICAL_SERVICE_ID, &service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status = validate_canonical_service(service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status =
            require_service(services, RODAQ_RUNTIME_PROJECTION_SERVICE_ID, &service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status = validate_projection_service(service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status =
            require_service(services, RODAQ_RUNTIME_ACQUISITION_SERVICE_ID, &service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status = validate_acquisition_service(service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status =
            require_service(services, RODAQ_RUNTIME_PROJECTION_SERVICE_ID, &service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        const auto* projection = static_cast<const rodaq_runtime_projection_v1*>(service.vtable);
        const char payload[] = {'f', 'i', 'x', 't', 'u', 'r', 'e'};
        rodaq_projection_registration registration{};
        registration.struct_size = RODAQ_PROJECTION_REGISTRATION_MIN_STRUCT_SIZE;
        registration.descriptor = rodaq_stream_descriptor_handle{42};
        const char projection_id[] = "rodaq.test.projection.v1";
        registration.projection_id = rodaq_string_view{projection_id, sizeof(projection_id) - 1};
        registration.payload =
            rodaq_bytes_view{reinterpret_cast<const std::uint8_t*>(payload), sizeof(payload)};
        status = projection->register_projection(service.context, &registration, out_error);
    }
    if (status != RODAQ_STATUS_OK) {
        fixture_record_event("bind-failure");
    } else {
        static_cast<FixtureInstanceState*>(instance)->bound = true;
    }
    return status;
}

rodaq_status fixture_acquisition_configure(
    void* instance, rodaq_config_view config, rodaq_error* out_error) noexcept {
    fixture_record_event("configure");
    rodaq_status status =
        validate_fixture_config(config, static_cast<FixtureInstanceState*>(instance), out_error);
    if (status != RODAQ_STATUS_OK) {
        fixture_record_event("configure-failure");
    }
    return status;
}

rodaq_status fixture_acquisition_start(void*, rodaq_error*) noexcept { return RODAQ_STATUS_OK; }

rodaq_status fixture_acquisition_stop(void*, rodaq_error*) noexcept { return RODAQ_STATUS_OK; }

rodaq_status fixture_storage_bind(
    void* instance, const rodaq_service_registry* services, rodaq_error* out_error) noexcept {
    fixture_record_event("bind");
    rodaq_interface service{};
    rodaq_status status =
        require_service(services, RODAQ_RUNTIME_COMMON_SERVICE_ID, &service, out_error);
    if (status == RODAQ_STATUS_OK) {
        status = validate_common_service(service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status = require_service(services, RODAQ_RUNTIME_CANONICAL_SERVICE_ID, &service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status = validate_canonical_service(service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status =
            require_service(services, RODAQ_RUNTIME_PROJECTION_SERVICE_ID, &service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status = validate_projection_service(service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        const auto* projection = static_cast<const rodaq_runtime_projection_v1*>(service.vtable);
        rodaq_projection_view projection_view{};
        projection_view.struct_size = RODAQ_PROJECTION_VIEW_MIN_STRUCT_SIZE;
        const char id[] = "rodaq.test.projection.v1";
        status = projection->get_projection(
            service.context, rodaq_stream_descriptor_handle{42},
            rodaq_string_view{id, sizeof(id) - 1}, &projection_view, out_error);
        if (status == RODAQ_STATUS_OK &&
            (projection_view.payload.size != 7U ||
             std::memcmp(projection_view.payload.data, "fixture", 7) != 0)) {
            status =
                fail(RODAQ_STATUS_INVALID_ARGUMENT, "projection fixture bytes differ", out_error);
        }
    }
    if (status == RODAQ_STATUS_OK) {
        status = require_service(
            services, RODAQ_RUNTIME_CLOCK_CORRELATION_SERVICE_ID, &service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status = validate_clock_service(service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        const auto* clock_service =
            static_cast<const rodaq_runtime_clock_correlation_v1*>(service.vtable);
        rodaq_unix_epoch_clock_anchor anchor{};
        anchor.struct_size = RODAQ_UNIX_EPOCH_CLOCK_ANCHOR_MIN_STRUCT_SIZE;
        status = clock_service->get_unix_epoch_anchor(
            service.context, rodaq_clock_handle{11}, &anchor, out_error);
        if (status == RODAQ_STATUS_OK && anchor.unix_anchor_ns != 1'000U) {
            status = fail(RODAQ_STATUS_INVALID_ARGUMENT, "clock fixture anchor differs", out_error);
        }
    }
    if (status == RODAQ_STATUS_OK) {
        status = require_service(services, RODAQ_RUNTIME_STORAGE_SERVICE_ID, &service, out_error);
    }
    if (status == RODAQ_STATUS_OK) {
        status = validate_storage_service(service, out_error);
    }
    if (status != RODAQ_STATUS_OK) {
        fixture_record_event("bind-failure");
    } else {
        static_cast<FixtureInstanceState*>(instance)->bound = true;
    }
    return status;
}

rodaq_status fixture_storage_configure(
    void* instance, rodaq_config_view config, rodaq_error* out_error) noexcept {
    fixture_record_event("configure");
    rodaq_status status =
        validate_fixture_config(config, static_cast<FixtureInstanceState*>(instance), out_error);
    if (status != RODAQ_STATUS_OK) {
        fixture_record_event("configure-failure");
    }
    return status;
}

rodaq_status fixture_storage_open(void*, const rodaq_storage_open_request*, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status fixture_storage_submit(
    void*, const rodaq_storage_submission*, rodaq_storage_submit_result*, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status fixture_storage_flush(void*, rodaq_error*) noexcept { return RODAQ_STATUS_OK; }

rodaq_status fixture_storage_close(
    void*, const rodaq_storage_close_request*, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status fixture_acquisition_query_interface(
    void* instance, rodaq_string_view interface_id, rodaq_interface* out_interface,
    rodaq_error* out_error) noexcept {
    static_cast<void>(instance);
    if (out_interface == nullptr) {
        return fail(RODAQ_STATUS_INVALID_ARGUMENT, "interface output is null", out_error);
    }
    *out_interface = {};
    const char expected[] = RODAQ_ACQUISITION_CONTROL_INTERFACE_ID;
    if (!string_equals(interface_id, expected, sizeof(expected) - 1)) {
        return fail(RODAQ_STATUS_NOT_FOUND, "interface is unknown", out_error);
    }
    out_interface->context =
#if RODAQ_FIXTURE_CONTROL_NULL_CONTEXT
        nullptr;
#else
        instance;
#endif
    out_interface->vtable = &acquisition_control.base;
    return RODAQ_STATUS_OK;
}

rodaq_status fixture_storage_query_interface(
    void* instance, rodaq_string_view interface_id, rodaq_interface* out_interface,
    rodaq_error* out_error) noexcept {
    if (out_interface == nullptr) {
        return fail(RODAQ_STATUS_INVALID_ARGUMENT, "interface output is null", out_error);
    }
    *out_interface = {};
    const char expected[] = RODAQ_STORAGE_CONTROL_INTERFACE_ID;
    if (!string_equals(interface_id, expected, sizeof(expected) - 1)) {
        return fail(RODAQ_STATUS_NOT_FOUND, "interface is unknown", out_error);
    }
    out_interface->context = instance;
    out_interface->vtable = &storage_control;
    return RODAQ_STATUS_OK;
}

}  // namespace rodaq::test
