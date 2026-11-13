#include <rodaq/plugin/c/abi.h>
#include <rodaq/plugin/c/interface.h>

#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(rodaq_status_code_t) == 4u, "status code width");
_Static_assert(RODAQ_STATUS_OK == 0u, "status value");
_Static_assert(offsetof(rodaq_plugin_instance_t, context) == 0u, "instance layout");
_Static_assert(RODAQ_PLUGIN_ABI_MAJOR(RODAQ_PLUGIN_ABI_VERSION(1u, 2u)) == 1u, "ABI major");
_Static_assert(RODAQ_PLUGIN_ABI_MINOR(RODAQ_PLUGIN_ABI_VERSION(1u, 2u)) == 2u, "ABI minor");

int main(void) {
    rodaq_plugin_instance_t instance = {NULL, NULL};
    rodaq_interface_reference_t reference = {NULL, NULL, 0u, {0u, 0u}};
    return instance.context == NULL && reference.vtable == NULL ? 0 : 1;
}
