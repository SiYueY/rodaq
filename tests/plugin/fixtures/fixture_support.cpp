#include "fixture_support.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace rodaq::test {
namespace {

class FixtureLifecycleObserver final {
public:
    FixtureLifecycleObserver() { fixture_record_event("load"); }

    ~FixtureLifecycleObserver() { fixture_record_event("unload"); }
};

FixtureLifecycleObserver lifecycle_observer;
int instance_storage = 0;

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
    static_cast<void>(write(descriptor, event, std::strlen(event)));
    static_cast<void>(write(descriptor, "\n", 1));
    static_cast<void>(close(descriptor));
}

rodaq_status fixture_create(void** out_instance, rodaq_error*) noexcept {
    fixture_record_event("create");
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

rodaq_status fixture_acquisition_bind(void*, const rodaq_service_registry*, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status fixture_acquisition_configure(void*, rodaq_config_view, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status fixture_acquisition_start(void*, rodaq_error*) noexcept { return RODAQ_STATUS_OK; }

rodaq_status fixture_acquisition_stop(void*, rodaq_error*) noexcept { return RODAQ_STATUS_OK; }

rodaq_status fixture_storage_bind(void*, const rodaq_service_registry*, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status fixture_storage_configure(void*, rodaq_config_view, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
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

rodaq_status fixture_query_interface(
    void*, rodaq_string_view, rodaq_interface* out_interface, rodaq_error*) noexcept {
    if (out_interface != nullptr) {
        out_interface->context = nullptr;
        out_interface->vtable = nullptr;
    }
    return RODAQ_STATUS_NOT_FOUND;
}

}  // namespace rodaq::test
