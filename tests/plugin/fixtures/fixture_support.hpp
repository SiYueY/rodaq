#ifndef RODAQ_TESTS_PLUGIN_FIXTURE_SUPPORT_HPP_
#define RODAQ_TESTS_PLUGIN_FIXTURE_SUPPORT_HPP_

#include <rodaq/plugin/abi/acquisition.h>
#include <rodaq/plugin/abi/storage.h>

#include <cstdint>

namespace rodaq::test {

void fixture_record_event(const char* event) noexcept;

rodaq_status fixture_create(void** out_instance, rodaq_error* out_error) noexcept;
void fixture_destroy(void* instance) noexcept;
rodaq_status fixture_create_failure(void** out_instance, rodaq_error* out_error) noexcept;
rodaq_status fixture_create_null(void** out_instance, rodaq_error* out_error) noexcept;

rodaq_status fixture_acquisition_bind(
    void* instance, const rodaq_service_registry* services, rodaq_error* out_error) noexcept;
rodaq_status fixture_acquisition_configure(
    void* instance, rodaq_config_view config, rodaq_error* out_error) noexcept;
rodaq_status fixture_acquisition_start(void* instance, rodaq_error* out_error) noexcept;
rodaq_status fixture_acquisition_stop(void* instance, rodaq_error* out_error) noexcept;

rodaq_status fixture_storage_bind(
    void* instance, const rodaq_service_registry* services, rodaq_error* out_error) noexcept;
rodaq_status fixture_storage_configure(
    void* instance, rodaq_config_view config, rodaq_error* out_error) noexcept;
rodaq_status fixture_storage_open(
    void* instance, const rodaq_storage_open_request* request, rodaq_error* out_error) noexcept;
rodaq_status fixture_storage_submit(
    void* instance, const rodaq_storage_submission* submission,
    rodaq_storage_submit_result* out_result, rodaq_error* out_error) noexcept;
rodaq_status fixture_storage_flush(void* instance, rodaq_error* out_error) noexcept;
rodaq_status fixture_storage_close(
    void* instance, const rodaq_storage_close_request* request, rodaq_error* out_error) noexcept;

rodaq_status fixture_query_interface(
    void* instance, rodaq_string_view interface_id, rodaq_interface* out_interface,
    rodaq_error* out_error) noexcept;

}  // namespace rodaq::test

#define RODAQ_FIXTURE_VIEW(value) \
    { value, sizeof(value) - 1 }

#endif  // RODAQ_TESTS_PLUGIN_FIXTURE_SUPPORT_HPP_
